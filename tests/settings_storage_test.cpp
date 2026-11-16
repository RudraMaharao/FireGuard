#include "settings.h"
#include "nvs.h"
#include "settings_store.h"
#include <assert.h>
#include <iostream>
#include <string.h>
using settings::Config;
void seed(const char *partition,const Config &c){
 nvs_handle_t h;assert(nvs_open_from_partition(partition,"fg-settings",NVS_READWRITE,&h)==ESP_OK);
 assert(nvs_set_blob(h,"config",&c,sizeof c)==ESP_OK);assert(nvs_commit(h)==ESP_OK);nvs_close(h);
}
int main(){
 Config legacy;
 strcpy(legacy.ssid,"Saved Home");strcpy(legacy.password,"legacy-password");strcpy(legacy.token,"123:Legacy_bot");
 strcpy(legacy.pin,"123456");strcpy(legacy.emergencyPin,"654321");strcpy(legacy.setupPassword,"fg-unchanged");strcpy(legacy.home,"Legacy home");
 legacy.contacts[0].chat=1234;legacy.emergencyChat=5678;
 seed("nvs",legacy);auto original=nvs_data.at("nvs/fg-settings/config");
 nvs_full_partition="nvs"; // old shared partition cannot accept another write
 settings::init();assert(!strcmp(settings::get().ssid,"Saved Home"));
 assert(!strcmp(settings::get().setupPassword,"fg-unchanged"));
 assert(settings::get().contacts[0].chat==1234 && settings::get().emergencyChat==5678);
 Config migrated;memcpy(&migrated,nvs_data.at("fg_config/fg-settings/config").data(),sizeof migrated);
 assert(!strcmp(migrated.ssid,legacy.ssid) && !strcmp(migrated.token,legacy.token));
 assert(!strcmp(migrated.password,legacy.password) && !strcmp(migrated.setupPassword,legacy.setupPassword));
 assert(migrated.contacts[0].chat==legacy.contacts[0].chat && migrated.emergencyChat==legacy.emergencyChat);
 assert(nvs_data.at("nvs/fg-settings/config")==original);
 std::string error;bool networkChanged;
 for(int i=0;i<100;i++){
  std::string data="{\"home\":\"Updated "+std::to_string(i)+"\"}";
  assert(settings::parse_save(data.c_str(),error,&networkChanged));assert(!networkChanged);
 }
 settings::init();assert(!strcmp(settings::get().home,"Updated 99")); // new partition wins on reboot
 assert(!strcmp(settings::get().password,"legacy-password"));
 nvs_full_partition="fg_config";
 assert(!settings::parse_save(R"({"home":"Should not publish"})",error,&networkChanged));
 assert(error.find("ESP_ERR_NVS_NOT_ENOUGH_SPACE")!=std::string::npos && !networkChanged);
 assert(!strcmp(settings::get().home,"Updated 99"));
 assert(nvs_data.at("nvs/fg-settings/config")==original);
 nvs_full_partition="nvs";assert(settings::parse_save(R"({"home":"Recovered"})",error));
 // Failed migration or initialization must leave legacy settings usable and untouched.
 nvs_data.erase("fg_config/fg-settings/config");nvs_full_partition="fg_config";
 settings::init();assert(!strcmp(settings::get().home,"Legacy home"));
 assert(nvs_data.at("nvs/fg-settings/config")==original);
 nvs_partition_init_result=ESP_ERR_NVS_NOT_INITIALIZED;settings::init();
 assert(!strcmp(settings::get().token,"123:Legacy_bot"));
 assert(!settings::parse_save(R"({"home":"No storage"})",error));
 assert(error.find("ESP_ERR_NVS_NOT_INITIALIZED")!=std::string::npos);
 assert(nvs_data.at("nvs/fg-settings/config")==original);
 // Per-key fallback retains old optional values even after another key is saved in new storage.
 nvs_partition_init_result=ESP_OK;nvs_full_partition.clear();
 nvs_handle_t h;int value=12,read=0;size_t size=sizeof read;
 assert(nvs_open("fg-sms",NVS_READWRITE,&h)==ESP_OK);
 assert(nvs_set_blob(h,"opts",&value,sizeof value)==ESP_OK);nvs_close(h);
 assert(settings_store::read_blob("fg-sms","opts",&read,&size)==ESP_OK && read==12);
 assert(settings_store::open_write("fg-sms",&h)==ESP_OK);value=34;
 assert(nvs_set_blob(h,"station",&value,sizeof value)==ESP_OK);nvs_close(h);
 size=sizeof read;assert(settings_store::read_blob("fg-sms","opts",&read,&size)==ESP_OK && read==12);
 assert(settings_store::open_write("fg-sms",&h)==ESP_OK);value=56;
 assert(nvs_set_blob(h,"opts",&value,sizeof value)==ESP_OK);nvs_close(h);
 size=sizeof read;assert(settings_store::read_blob("fg-sms","opts",&read,&size)==ESP_OK && read==56);
 assert(nvs_handles.empty());
 std::cout<<"Settings migration, namespace isolation, 100 repeated saves, reboot precedence, capacity/init failure and legacy preservation passed\n";
}
