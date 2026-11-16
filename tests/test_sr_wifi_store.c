#include "sr_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct { char key[16]; char value[SR_PASS_CAP]; } kv[32];
static int count;
static const char *find(const char *key) { for(int i=0;i<count;i++) if(!strcmp(kv[i].key,key)) return kv[i].value; return NULL; }
esp_err_t nvs_open(const char *n,nvs_open_mode_t m,nvs_handle_t *h) {(void)n;(void)m;*h=1;return ESP_OK;}
void nvs_close(nvs_handle_t h) {(void)h;}
esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *v,size_t *n) {(void)h;const char *s=find(k);if(!s)return ESP_ERR_NVS_NOT_FOUND;size_t len=strlen(s)+1;if(*n<len)return ESP_FAIL;memcpy(v,s,len);*n=len;return ESP_OK;}
esp_err_t nvs_set_str(nvs_handle_t h,const char *k,const char *v) {(void)h;int i=0;while(i<count&&strcmp(kv[i].key,k))i++;if(i==count){assert(count<32);count++;}snprintf(kv[i].key,sizeof(kv[i].key),"%s",k);snprintf(kv[i].value,sizeof(kv[i].value),"%s",v);return ESP_OK;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v){(void)h;(void)k;(void)v;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_get_u16(nvs_handle_t h,const char *k,uint16_t *v){(void)h;(void)k;(void)v;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_get_i64(nvs_handle_t h,const char *k,int64_t *v){(void)h;(void)k;(void)v;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char *k,uint8_t v){(void)h;(void)k;(void)v;return ESP_OK;}
esp_err_t nvs_set_u16(nvs_handle_t h,const char *k,uint16_t v){(void)h;(void)k;(void)v;return ESP_OK;}
esp_err_t nvs_set_i64(nvs_handle_t h,const char *k,int64_t v){(void)h;(void)k;(void)v;return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){(void)h;return ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *k){(void)h;(void)k;return ESP_OK;}
esp_err_t nvs_erase_all(nvs_handle_t h){(void)h;return ESP_OK;}
int main(void) {
 sr_wifi_entry_t list[SR_WIFI_MAX]={0};
 assert(nvs_set_str(1,"ssid","old") == ESP_OK); assert(nvs_set_str(1,"wpass","oldpass")==ESP_OK);
 assert(nvs_set_str(1,"rtok","auth-token")==ESP_OK);
 assert(sr_store_list_networks(list,SR_WIFI_MAX)==1 && !strcmp(list[0].ssid,"old"));
 assert(sr_store_add_network("backup","secret",false)==ESP_OK);
 assert(!strcmp(find("ssid"),"old") && !strcmp(find("wpass"),"oldpass"));
 assert(sr_store_list_networks(list,SR_WIFI_MAX)==2);
 assert(sr_store_save_network("backup","secret")==ESP_OK);
 assert(!strcmp(find("ssid"),"backup") && !strcmp(find("rtok"),"auth-token"));
 assert(sr_store_add_network("backup","updated",false)==ESP_OK);
 assert(sr_store_list_networks(list,SR_WIFI_MAX)==2 && !strcmp(list[1].pass,"updated"));
 assert(sr_store_add_network("third","",false)==ESP_OK);
 assert(sr_store_add_network("fourth","pass",false)==ESP_OK);
 assert(sr_store_add_network("fifth","pass",false)!=ESP_OK);
 assert(!strcmp(find("ssid"),"backup") && !strcmp(find("rtok"),"auth-token"));
 assert(sr_store_list_networks(list,SR_WIFI_MAX)==4);
 assert(sr_store_add_network("", "",false)==ESP_ERR_INVALID_ARG);
 puts("multi wifi storage and legacy migration PASS");
 return 0;
}
