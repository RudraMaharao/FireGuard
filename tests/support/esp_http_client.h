#pragma once
// Records the last HTTPS request so host tests can check what would be sent.
#include "esp_err.h"
#include <map>
#include <string>
enum esp_http_client_method_t{HTTP_METHOD_GET,HTTP_METHOD_POST};
enum esp_http_client_event_id_t{HTTP_EVENT_ERROR,HTTP_EVENT_ON_CONNECTED,HTTP_EVENT_HEADERS_SENT,HTTP_EVENT_ON_HEADER,HTTP_EVENT_ON_DATA,HTTP_EVENT_ON_FINISH,HTTP_EVENT_DISCONNECTED};
struct esp_http_client_event_t{esp_http_client_event_id_t event_id;void*client;void*data;int data_len;void*user_data;};
struct esp_http_client_config_t{const char*url=nullptr;esp_err_t(*crt_bundle_attach)(void*)=nullptr;int timeout_ms=0;int buffer_size=0;bool disable_auto_redirect=false;esp_err_t(*event_handler)(esp_http_client_event_t*)=nullptr;void*user_data=nullptr;};
struct FakeHttp{std::string url,body;std::map<std::string,std::string> headers;esp_http_client_method_t method=HTTP_METHOD_GET;esp_err_t(*handler)(esp_http_client_event_t*)=nullptr;void*user=nullptr;};
using esp_http_client_handle_t=FakeHttp*;
inline void(*fake_http_hook)()=nullptr;
inline FakeHttp fake_http_last;inline int fake_http_status=200,fake_http_requests=0;inline std::string fake_http_response; // body delivered to the event handler
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*c){auto*h=new FakeHttp;h->url=c->url;h->handler=c->event_handler;h->user=c->user_data;return h;}
inline void esp_http_client_set_method(esp_http_client_handle_t h,esp_http_client_method_t m){h->method=m;}
inline void esp_http_client_set_header(esp_http_client_handle_t h,const char*k,const char*v){h->headers[k]=v;}
inline void esp_http_client_set_post_field(esp_http_client_handle_t h,const char*d,int n){h->body.assign(d,n);}
inline esp_err_t esp_http_client_perform(esp_http_client_handle_t h){fake_http_last=*h;fake_http_requests++;if(fake_http_hook){auto hook=fake_http_hook;fake_http_hook=nullptr;hook();}if(h->handler&&!fake_http_response.empty()){esp_http_client_event_t e{HTTP_EVENT_ON_DATA,h,(void*)fake_http_response.data(),int(fake_http_response.size()),h->user};h->handler(&e);}return ESP_OK;}
inline int esp_http_client_get_status_code(esp_http_client_handle_t){return fake_http_status;}
inline void esp_http_client_cleanup(esp_http_client_handle_t h){delete h;}
