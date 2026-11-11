#pragma once
#include <string>
// Fixed destination, verified hostname/CA, bounded POST and response. Runs on Wi-Fi task.
int wifiCloudPost(const std::string& clock,const std::string& token,const std::string& body,std::string& response);
