// Don't modify this file as it will be overwritten.
//
#include "IDL_UserDataType.h"

UserDataType::UserDataType(const UserDataType &IDL_s){
  a = IDL_s.a;
  sent_packets = IDL_s.sent_packets;
  MD5 = IDL_s.MD5;
}

UserDataType& UserDataType::operator= (const UserDataType &IDL_s){
  if (this == &IDL_s) return *this;
  a = IDL_s.a;
  sent_packets = IDL_s.sent_packets;
  MD5 = IDL_s.MD5;
  return *this;
}

void UserDataType::Marshal(CDR *cdr) const {
  cdr->PutString(a);
  cdr->PutLong(sent_packets);
  cdr->PutString(MD5);
}

void UserDataType::UnMarshal(CDR *cdr){
  {
    char *IDL_str;
    cdr->GetString(IDL_str);
    if(a != NULL )
    {
        delete a;
        a = NULL;
    }
    a = IDL_str;
  }
  cdr->GetLong(sent_packets);
  {
    char *IDL_str;
    cdr->GetString(IDL_str);
    if(MD5 != NULL )
    {
        delete MD5;
        MD5 = NULL;
    }
    MD5 = IDL_str;
  }
}

