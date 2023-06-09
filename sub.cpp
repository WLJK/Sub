/**********************************************************
*****************订阅端程序subscriber.cpp*******************
***********************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <openssl/md5.h>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

/* IDL_TypeSupport.h中包含所有依赖的头文件 */
#include "IDL_TypeSupport.h"

std::condition_variable reply_received_condition;
std::mutex reply_mutex;


std::atomic<int> throughput_counter(0);
std::mutex counter_mutex;
static int data_size = 1024;
static long sent_packets = 0;  // 发送的数据包数量
static long received_packets = 0;// 接收到的数据包数量
static long before_sent = 0;    // 上次计算丢包发送数量

//std::queue<UserDataType> data_queue;  // 数据队列
//std::mutex queue_mutex;  // 互斥锁，用于保护数据队列的访问
//std::condition_variable queue_condition;  // 条件变量，用于线程间的通信
//使用OpenSSL库计算MD5哈希
    static std::string calculate_MD5(const std::string &input) {
    unsigned char hash[MD5_DIGEST_LENGTH];
    MD5((unsigned char*)input.c_str(), input.size(), hash);

    std::ostringstream ss;
    for(int i = 0; i < MD5_DIGEST_LENGTH; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];

    return ss.str();
}

/* UserDataTypeListener继承于DataReaderListener，
   需要重写其继承过来的方法on_data_available()，在其中进行数据监听读取操作 */
class UserDataTypeListener : public DataReaderListener {
public:
	void on_data_available(DataReader* reader) override;
};

//auto pool = new thread_pool<ReadDataTask>();
/* 重写继承过来的方法on_data_available()，在其中进行数据监听读取操作 */
void UserDataTypeListener::on_data_available(DataReader* reader)
{
	UserDataTypeDataReader *UserDataType_reader = nullptr;
	UserDataTypeSeq data_seq;
	SampleInfoSeq info_seq;
	ReturnCode_t retcode;
	int i;

	/* 利用reader，创建一个读取UserDataType类型的UserDataType_reader*/
	UserDataType_reader = UserDataTypeDataReader::narrow(reader);
	if (UserDataType_reader == nullptr) {
		fprintf(stderr, "DataReader narrow error\n");
		return;
	}

	/* 获取数据，存放至data_seq，data_seq是一个队列 */
	retcode = UserDataType_reader->read(
		data_seq, info_seq, 10, 0, 0, 0);

	if (retcode == RETCODE_NO_DATA) {
		return;
	}
	else if (retcode != RETCODE_OK) {
		fprintf(stderr, "take error %d\n", retcode);
		return;
	}

    for (i = 0; i < data_seq.length(); ++i) {
        //auto task = std::make_shared<ReadDataTask>(data_seq[i]);
        //UserDataType* data_read = new UserDataType(data_seq[i]);
        //pool->append(new ReadDataTask(data_read));

        // 获取从发送端接收到的数据和MD5
        char* data = data_seq[i].a;
        char* received_MD5 = data_seq[i].MD5;
        long received_sent_packets = data_seq[i].sent_packets; // 接收到的计数器值

        // 计算接收到的数据的MD5哈希
        std::string calculated_MD5 = calculate_MD5(std::string(data, data_size));

        // 将接收到的MD5转换为字符串，以便比较
        std::string received_MD5_str(received_MD5);

        // 比较接收到的MD5哈希和计算得到的MD5哈希
        if (calculated_MD5 != received_MD5_str) {
            std::cerr << "MD5 不一致 " << received_MD5_str
                      << ", calculated " << calculated_MD5 << std::endl;
        }

        // 释放内存
        delete[] data_seq[i].a;
        data_seq[i].a = nullptr;

        delete[] data_seq[i].MD5;
        data_seq[i].MD5 = nullptr;

        ++received_packets; // 接收到的数据包数量
        sent_packets = received_sent_packets; // 更新发送的数据包数量
        ++throughput_counter;

        // 发送回复信号
        std::lock_guard<std::mutex> reply_lock(reply_mutex);
        reply_received_condition.notify_all();


        // 将数据放入队列
       /* {
            std::lock_guard<std::mutex> lock(queue_mutex);
            data_queue.push(data_seq[i]);
        }

        // 通知数据处理线程有新数据可处理
        queue_condition.notify_all();   */
    }

}

/* 删除所有实体 */
static int subscriber_shutdown(
	DomainParticipant *participant)
{
	ReturnCode_t retcode;
	int status = 0;

	if (participant != nullptr) {
		retcode = participant->delete_contained_entities();
		if (retcode != RETCODE_OK) {
			fprintf(stderr, "delete_contained_entities error %d\n", retcode);
			status = -1;
		}

		retcode = DomainParticipantFactory::get_instance()->delete_participant(participant);
		if (retcode != RETCODE_OK) {
			fprintf(stderr, "delete_participant error %d\n", retcode);
			status = -1;
		}
	}
	return status;
}

/* 订阅者函数 */
extern "C" int subscriber_main(int domainId, int sample_count, int data_size)
{
	DomainParticipant *participant = nullptr;
    Publisher *publisher = NULL;
	Subscriber *subscriber = nullptr;
    Topic* publish_topic = NULL;
    Topic* subscribe_topic = NULL;
	UserDataTypeListener *reader_listener = nullptr;
	DataReader *reader = nullptr;
    DataWriter *writer = NULL;
    UserDataType *instance = NULL;
    InstanceHandle_t instance_handle = HANDLE_NIL;
    UserDataTypeDataWriter * UserDataType_writer = NULL;
	ReturnCode_t retcode;
	const char *type_name = nullptr;
	int count = 0;
	int status = 0;

	/* 1. 创建一个participant，可以在此处定制participant的QoS */
	/* 建议1：在程序启动后优先创建participant，进行资源初始化*/
	/* 建议2：相同的domainId只创建一次participant，重复创建会占用大量资源 */
	participant = DomainParticipantFactory::get_instance()->create_participant(
		domainId, PARTICIPANT_QOS_DEFAULT/* participant默认QoS */,
        nullptr /* listener */, STATUS_MASK_NONE);
	if (participant == nullptr) {
		fprintf(stderr, "create_participant error\n");
		subscriber_shutdown(participant);
		return -1;
	}

	/* 2. 创建一个subscriber，可以在创建subscriber的同时定制其QoS  */
	/* 建议1：在程序启动后优先创建subscriber*/
	/* 建议2：一个participant下创建一个subscriber即可，无需重复创建 */
	subscriber = participant->create_subscriber(
		SUBSCRIBER_QOS_DEFAULT/* 默认QoS */,
        nullptr /* listener */, STATUS_MASK_NONE);
	if (subscriber == nullptr) {
		fprintf(stderr, "create_subscriber error\n");
		subscriber_shutdown(participant);
		return -1;
	}

    publisher = participant->create_publisher(
            PUBLISHER_QOS_DEFAULT /* 默认QoS */,
            NULL /* listener */, STATUS_MASK_NONE);
    if (publisher == NULL) {
        fprintf(stderr, "create_publisher error\n");
        subscriber_shutdown(participant);
        return -1;
    }

	/* 3. 在创建主题之前注册数据类型 */
	/* 建议1：在程序启动后优先进行注册 */
	/* 建议2：一个数据类型注册一次即可 */
	type_name = UserDataTypeTypeSupport::get_type_name();
	retcode = UserDataTypeTypeSupport::register_type(
		participant, type_name);
	if (retcode != RETCODE_OK) {
		fprintf(stderr, "register_type error %d\n", retcode);
		subscriber_shutdown(participant);
		return -1;
	}

	/* 4. 创建主题，并定制主题的QoS  */
	/* 建议1：在程序启动后优先创建Topic */
	/* 建议2：一种主题名创建一次即可，无需重复创建 */
    subscribe_topic = participant->create_topic(
		"Topic_A"/* 主题名，应与发布者主题名一致 */,
		type_name, TOPIC_QOS_DEFAULT/* 默认QoS */,
        nullptr /* listener */, STATUS_MASK_NONE);
	if (subscribe_topic == nullptr) {
		fprintf(stderr, "create_topic error\n");
		subscriber_shutdown(participant);
		return -1;
	}

    publish_topic = participant->create_topic(
            "topic_B" /* 订阅主题名 */,
            type_name /* 类型名 */, TOPIC_QOS_DEFAULT /* 默认QoS */,
            NULL /* listener */, STATUS_MASK_NONE);
    if (publish_topic == NULL) {
        fprintf(stderr, "create_topic for subscribe error\n");
        subscriber_shutdown(participant);
        return -1;
    }

	/* 5. 创建一个监听器 */
	reader_listener = new UserDataTypeListener();

    /* 6. 创建datareader，并定制datareader的QoS */

    reader = subscriber->create_datareader(
            subscribe_topic, DATAREADER_QOS_DEFAULT/* 默认QoS */,
            reader_listener/* listener */, STATUS_MASK_ALL);
    if (reader == nullptr) {
        fprintf(stderr, "create_datareader error\n");
        subscriber_shutdown(participant);
        delete reader_listener;
        return -1;
    }

    writer = publisher->create_datawriter(
            publish_topic , DATAWRITER_QOS_DEFAULT/* 默认QoS */,
            NULL /* listener */, STATUS_MASK_NONE);
    if (writer == NULL) {
        fprintf(stderr, "create_datawriter error\n");
        subscriber_shutdown(participant);
        return -1;
    }
    UserDataType_writer = UserDataTypeDataWriter::narrow(writer);
    if (UserDataType_writer == NULL) {
        fprintf(stderr, "DataWriter narrow error\n");
        subscriber_shutdown(participant);
        return -1;
    }

    // 创建数据处理线程
    /*  std::vector<std::thread> processing_threads;
    int num_processing_threads = std::thread::hardware_concurrency();
    for (int i = 0; i < num_processing_threads; ++i) {
        processing_threads.emplace_back(dataProcessingThread);
    }

     /* 6. 创建一个数据样本 */
    /* 建议：该数据为new出来的，使用后用户需要调用delete_data进行释放内存*/
    instance = UserDataTypeTypeSupport::create_data();
    if (instance == NULL) {
        fprintf(stderr, "UserDataTypeTypeSupport::create_data error\n");
        subscriber_shutdown(participant);
        return -1;
    }
    char data_put = 1;
    /* 7. 主循环 ，监听器会默认调用on_data_available()监听数据 */
	for (count = 0; (sample_count == 0) || (count < sample_count); ++count) {
        // 等待回复
        std::unique_lock<std::mutex> reply_lock(reply_mutex);
        reply_received_condition.wait(reply_lock);

        instance->a = new char[data_size];
        memset(instance->a, data_put, data_size);
        instance->MD5 = new char[33];

        std::string md5 = calculate_MD5(std::string(instance->a, data_size));
        strcpy(instance->MD5, md5.c_str());
        ++sent_packets;
        instance->sent_packets = sent_packets;

        // 发送数据
        retcode = UserDataType_writer->write(*instance, instance_handle);
        if (retcode != RETCODE_OK) {
            fprintf(stderr, "write error %d\n", retcode);
        }   else {std::cout << "发送成功" <<std::endl;}

	}

    // 等待所有任务完成
    //pool->wait_for_all_tasks();
    //delete pool;

    // 等待数据处理线程结束
    /*for (auto& thread : processing_threads) {
        thread.join();
    }

     /*  删除数据样本 */
    retcode = UserDataTypeTypeSupport::delete_data(instance);
    if (retcode != RETCODE_OK) {
        fprintf(stderr, "UserDataTypeTypeSupport::delete_data error %d\n", retcode);
    }

	/* 8. 删除所有实体和监听器 */
	status = subscriber_shutdown(participant);
	delete reader_listener;

	return status;
}

//简单定时吞吐量

void print_throughput(int data_size) {


    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        int current_sent_packets; // 计算值
        long current_received_packets;
        long current_throughput;

        {
            std::lock_guard<std::mutex> lock(counter_mutex);
            current_sent_packets = sent_packets - before_sent; // 发送的包
            current_received_packets = received_packets; // 接收到的数据包数量
            current_throughput = throughput_counter;    // 吞吐量
            before_sent = sent_packets; //记录值
            throughput_counter = 0;
            sent_packets = 0;
            received_packets = 0;   // 重置接收到的数据包数量
        }

        double packet_loss_rate = 0.0;
        if (current_sent_packets > 0) {
            packet_loss_rate = static_cast<double>(current_received_packets) / current_sent_packets;
        }

        std::cout << "订阅端吞吐量: " << data_size * 8 * current_throughput << " bit/s" << std::endl;
        std::cout << "丢包率: " << (1 - packet_loss_rate) * 100 << "%" << std::endl;
    }
}



int main(int argc, char *argv[])
{
	int domain_id = 0;

	int sample_count = 0; /* 无限循环 */

	if (argc >= 2) {
		domain_id = atoi(argv[1]);/* 发送至域domain_id */
	}
	if (argc >= 3) {
		sample_count = atoi(argv[2]);/* 发送sample_count次 */
	}
    if (argc >= 4) {
        data_size = atoi(argv[3]); /* 发送数据大小 */
        std::cout << "data_size  :" <<data_size<<std::endl;
    }
    // 创建定时器线程
    std::thread timer_thread(print_throughput, data_size);
    timer_thread.detach();
	return subscriber_main(domain_id, sample_count, data_size);
}

