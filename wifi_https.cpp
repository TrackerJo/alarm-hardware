#include "wifi_https.h"
#include "wifi_tls_root.h"
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <algorithm>
namespace {
constexpr char host[]="us-central1-kazoom-dawn.cloudfunctions.net";
int randomBytes(void*,unsigned char* out,size_t length){esp_fill_random(out,length);return 0;}
int sendBytes(void* ctx,const unsigned char* p,size_t n){int r=send(*static_cast<int*>(ctx),p,n,0);return r>=0?r:MBEDTLS_ERR_SSL_INTERNAL_ERROR;}
int readBytes(void* ctx,unsigned char* p,size_t n){int r=recv(*static_cast<int*>(ctx),p,n,0);return r>=0?r:MBEDTLS_ERR_SSL_INTERNAL_ERROR;}
int connectSocket(){
    addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;addrinfo* result=nullptr;
    if(getaddrinfo(host,"443",&hints,&result)!=0||!result)return -1;
    int fd=socket(result->ai_family,SOCK_STREAM,0);
    if(fd>=0){
        fcntl(fd,F_SETFL,O_NONBLOCK);int r=connect(fd,result->ai_addr,result->ai_addrlen);bool ok=r==0;
        if(r<0&&errno==EINPROGRESS){fd_set writable;FD_ZERO(&writable);FD_SET(fd,&writable);timeval timeout{5,0};ok=select(fd+1,nullptr,&writable,nullptr,&timeout)>0;}
        int error=-1;socklen_t length=sizeof error;
        ok=ok&&getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length)==0&&error==0;
        if(!ok){close(fd);fd=-1;}else{fcntl(fd,F_SETFL,0);timeval timeout{5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);}
    }
    freeaddrinfo(result);return fd;
}
}
int wifiCloudPost(const std::string& clock,const std::string& token,const std::string& body,std::string& response){
    if(body.size()>80000||token.size()!=64)return 0;
    int fd=connectSocket();if(fd<0)return 0;
    mbedtls_ssl_context ssl;mbedtls_ssl_config config;mbedtls_x509_crt ca;
    mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&config);mbedtls_x509_crt_init(&ca);
    bool ok=mbedtls_x509_crt_parse(&ca,reinterpret_cast<const uint8_t*>(WifiCloudRoot),sizeof WifiCloudRoot)==0&&
        mbedtls_ssl_config_defaults(&config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)==0;
    mbedtls_ssl_conf_authmode(&config,MBEDTLS_SSL_VERIFY_REQUIRED);mbedtls_ssl_conf_ca_chain(&config,&ca,nullptr);mbedtls_ssl_conf_rng(&config,randomBytes,nullptr);
    ok=ok&&mbedtls_ssl_setup(&ssl,&config)==0&&mbedtls_ssl_set_hostname(&ssl,host)==0;
    mbedtls_ssl_set_bio(&ssl,&fd,sendBytes,readBytes,nullptr);
    int64_t until=esp_timer_get_time()+30000000;
    if(ok){int r;do{r=mbedtls_ssl_handshake(&ssl);}while((r==MBEDTLS_ERR_SSL_WANT_READ||r==MBEDTLS_ERR_SSL_WANT_WRITE)&&esp_timer_get_time()<until);ok=r==0&&mbedtls_ssl_get_verify_result(&ssl)==0;}
    std::string request="POST /clockWifiSync HTTP/1.1\r\nHost: "+std::string(host)+"\r\nAuthorization: Bearer "+token+
        "\r\nX-Dawn-Clock: "+clock+"\r\nContent-Type: text/plain\r\nAccept-Encoding: identity\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
    size_t sent=0;
    while(ok&&sent<request.size()&&esp_timer_get_time()<until){int n=mbedtls_ssl_write(&ssl,reinterpret_cast<const uint8_t*>(request.data()+sent),std::min(size_t(4096),request.size()-sent));if(n>0)sent+=n;else if(n!=MBEDTLS_ERR_SSL_WANT_READ&&n!=MBEDTLS_ERR_SSL_WANT_WRITE)ok=false;}
    ok=ok&&sent==request.size();std::string raw;char buffer[768];size_t headerEnd=std::string::npos,expected=0;bool hasLength=false;
    while(ok&&esp_timer_get_time()<until){
        int n=mbedtls_ssl_read(&ssl,reinterpret_cast<uint8_t*>(buffer),sizeof buffer);
        if(n<=0){if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE)continue;break;}
        raw.append(buffer,n);if(raw.size()>44100){ok=false;break;}
        if(headerEnd==std::string::npos){
            headerEnd=raw.find("\r\n\r\n");if(headerEnd==std::string::npos){if(raw.size()>4096)ok=false;continue;}
            std::string headers=raw.substr(0,headerEnd);for(auto& c:headers)if(c>='A'&&c<='Z')c+=32;
            auto at=headers.find("\r\ncontent-length:");if(at!=std::string::npos){at+=17;while(at<headers.size()&&headers[at]==' ')++at;auto end=headers.find("\r\n",at);auto number=headers.substr(at,end==std::string::npos?end:end-at);uint64_t size=0;hasLength=!number.empty();for(char c:number){if(c<'0'||c>'9'||size>40000){hasLength=false;break;}size=size*10+uint64_t(c-'0');}expected=size;}
            if(!hasLength||expected>40000||headers.find("transfer-encoding:")!=std::string::npos){ok=false;break;}
        }
        if(headerEnd!=std::string::npos&&raw.size()>=headerEnd+4+expected)break;
    }
    int status=0;
    if(ok&&headerEnd!=std::string::npos&&raw.size()==headerEnd+4+expected&&raw.compare(0,9,"HTTP/1.1 ")==0&&raw.size()>12){
        const std::string code=raw.substr(9,3);if(code=="200"||code=="401"||code=="409"||code=="503"||code=="400")status=atoi(code.c_str());
        if(status==200)response=raw.substr(headerEnd+4);
    }
    mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&config);mbedtls_x509_crt_free(&ca);close(fd);return status;
}
