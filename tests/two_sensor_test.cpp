#include "ads1115.h"
#include "two_sensor_model.h"
#include "brain/fusion.h"
#include "esp_timer.h"
#include <cassert>
#include <iostream>
int main(){
 ads_mode=true;present_addr=0x48;Ads1115 a;assert(a.begin((void*)1));float mv;
 assert(a.readMv(0,mv)&&fabs(mv-1000)<.001);assert((ads_config&0x7fff)==0x4383);
 assert(a.readMv(2,mv)&&fabs(mv-2500)<.001);assert((ads_config&0x7fff)==0x6383);
 assert(!a.readMv(4,mv)&&isnan(mv));
 ads_codes[0]=uint16_t(-8);assert(!a.readMv(0,mv));ads_codes[0]=28000;assert(!a.readMv(0,mv));
 ads_codes[0]=8000;ads_busy=true;assert(!a.readMv(0,mv));ads_busy=false;
 fake_time_us+=6000000;assert(a.readMv(0,mv));
 i2c_fail=true;assert(!a.readMv(2,mv)&&isnan(mv));i2c_fail=false;
 assert(!a.readMv(2,mv));fake_time_us+=6000000;assert(a.readMv(2,mv));
 RelativeMQ q;assert(q.sample(2000)&&fabs(q.dev)<1e-6);assert(q.sample(3000)&&q.dev>0);
 auto baseline=q.baseline;q.updateBaseline(60,false);assert(q.baseline==baseline);
 assert(!q.sample(NAN)&&isnan(q.dev));assert(!q.sample(0));assert(!q.sample(5000));
 TwoSensorModel model;auto clear=model.run(0,0,0,0);assert(clear.gas==0&&clear.smoke==0&&clear.co==0);
 auto elevated=model.run(1,1,3,0);assert(elevated.gas==1&&elevated.smoke==1&&elevated.co==0);
 auto broken=model.run(NAN,1,3,0);assert(broken.gas==0&&broken.smoke==0);
 auto steam=model.run(.05,.5,0,3);assert(steam.steamVetoed);
 // Production fusion with the removed CO channel disabled: faults cannot hide a fire.
 Fusion f;f.begin(0);f.w[CH_CG]=0;
 for(unsigned ms=500;ms<120000;ms+=500){
  f.raw[CH_CG]=0;f.raw[CH_FV]=1;f.raw[CH_SG]=1;f.raw[CH_TE]=1;f.raw[CH_GG]=1;
  f.condition(.5);f.aggregate(.5);f.auxScores(false);f.updateState(ms,false,true);
 }
 assert(f.crs==0&&f.state==ST_CRITICAL);
 std::cout<<"ADS1115 mux, gain, signed scaling, timeout, fault recovery, two-sensor scoring and no-CO fusion passed\n";
}
