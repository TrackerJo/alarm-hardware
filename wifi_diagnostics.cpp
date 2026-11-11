// Optional USB diagnostics for prebuilt ESP32 EAP/TLS code. No credentials or
// packet contents are logged. Standard Arduino IDE builds compile this out.
#ifdef DAWN_WIFI_TLS_DIAGNOSTICS
#include <Arduino.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <esp_heap_caps.h>
struct eap_sm;
struct eap_method;
struct tls_config;
extern "C" int __real_eap_peer_blob_init(eap_sm*);
extern "C" int __wrap_eap_peer_blob_init(eap_sm* sm){int result=__real_eap_peer_blob_init(sm);Serial.printf("Wi-Fi EAP blob init: result=%d\n",result);return result;}
extern "C" int __real_eap_peer_config_init(eap_sm*,const uint8_t*,int);
extern "C" int __wrap_eap_peer_config_init(eap_sm* sm,const uint8_t* password,int length){int result=__real_eap_peer_config_init(sm,password,length);Serial.printf("Wi-Fi EAP config init: result=%d\n",result);return result;}
extern "C" const eap_method* __real_eap_peer_get_eap_method(int,int);
extern "C" const eap_method* __wrap_eap_peer_get_eap_method(int vendor,int method){const auto* result=__real_eap_peer_get_eap_method(vendor,method);Serial.printf("Wi-Fi EAP method: vendor=%d; type=%d; available=%d\n",vendor,method,result!=nullptr);return result;}
extern "C" void* __real_tls_init(const tls_config*);
extern "C" void* __wrap_tls_init(const tls_config* config){void* result=__real_tls_init(config);Serial.printf("Wi-Fi EAP TLS init: success=%d; free=%u\n",result!=nullptr,unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)));return result;}

extern "C" int __real_mbedtls_x509_crt_parse(mbedtls_x509_crt*,const unsigned char*,size_t);
extern "C" int __wrap_mbedtls_x509_crt_parse(mbedtls_x509_crt* chain,const unsigned char* bytes,size_t length){
    int result=__real_mbedtls_x509_crt_parse(chain,bytes,length);
    if(length>3000||result<0)Serial.printf("Wi-Fi TLS certificates: bytes=%u; result=%d; free=%u; largest=%u\n",unsigned(length),result,unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return result;
}
extern "C" int __real_mbedtls_ssl_setup(mbedtls_ssl_context*,const mbedtls_ssl_config*);
extern "C" int __real_mbedtls_ssl_handshake_step(mbedtls_ssl_context*);
extern "C" int __wrap_mbedtls_ssl_setup(mbedtls_ssl_context* ssl,const mbedtls_ssl_config* config){
    int result=__real_mbedtls_ssl_setup(ssl,config);
    Serial.printf("Wi-Fi TLS setup: result=-0x%x; free=%u; largest=%u\n",unsigned(-result),unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return result;
}
extern "C" int __wrap_mbedtls_ssl_handshake_step(mbedtls_ssl_context* ssl){
    int before=ssl->MBEDTLS_PRIVATE(state);
    int result=__real_mbedtls_ssl_handshake_step(ssl);
    Serial.printf("Wi-Fi TLS step: state=%d->%d; result=-0x%x; verify=0x%lx; free=%u; largest=%u\n",before,ssl->MBEDTLS_PRIVATE(state),unsigned(-result),static_cast<unsigned long>(mbedtls_ssl_get_verify_result(ssl)),unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return result;
}
#endif
