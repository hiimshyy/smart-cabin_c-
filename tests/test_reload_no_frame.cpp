#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "resident_catalog.h"
#include "resident_enroll.h"
#include "video_io.h"

static int checks=0,failed=0;
#define CHECK(c,m) do{++checks;if(!(c)){++failed;std::printf("  FAIL: %s (line %d)\n",m,__LINE__);}}while(0)
static double now_ms(){using namespace std::chrono;return duration<double,std::milli>(steady_clock::now().time_since_epoch()).count();}
static std::vector<float> unit(int n,float s){std::vector<float>v(n);double q=0;for(int i=0;i<n;++i){v[i]=s+i*.01f;q+=v[i]*v[i];}q=std::sqrt(q);for(float&x:v)x/=q;return v;}
static ResidentEnrollRequest request(const char* id,float seed){ResidentEnrollRequest r;r.ext_id=id;r.name=id;r.role="staff";r.replace=true;r.embeddings={unit(8,seed)};return r;}

int main(int argc,char**argv){
    const std::string schema=argc>1?argv[1]:"db/schema.sql";
    const std::string path="/tmp/test_reload_no_frame.db";
    std::remove(path.c_str());std::remove((path+"-wal").c_str());std::remove((path+"-shm").c_str());
    ResidentDB writer; CHECK(writer.open(path,schema,false),"open writer");
    CHECK(write_resident_enrollment(writer,request("A",1)).ok,"seed initial resident");

    ResidentCatalogReloader reload;std::string error;ResidentCatalogCandidate c;
    const double start=now_ms();
    CHECK(reload.open(path,8,50,start,error),"open 50ms reloader");
    CHECK(reload.poll(now_ms(),c,error)==ResidentCatalogReloader::PollResult::Reloaded,"initial reload");
    reload.accept(c);

    FrameSlot slot;std::atomic<bool>stop{false};uint64_t seq=0;cv::Mat frame;
    std::thread commit([&]{std::this_thread::sleep_for(std::chrono::milliseconds(10));write_resident_enrollment(writer,request("B",2));});
    const double before=now_ms();
    auto wr=wait_for_frame_or_deadline(&slot,&stop,&seq,&frame,
        std::max(0.0,reload.next_deadline_ms()-now_ms()));
    const double waited=now_ms()-before;
    commit.join();
    CHECK(wr==FrameWaitResult::Deadline,"camera outage reaches DB poll deadline");
    CHECK(waited>=25 && waited<300,"no-frame deadline neither busy-loops nor hangs");
    CHECK(reload.poll(now_ms(),c,error)==ResidentCatalogReloader::PollResult::Reloaded,
          "external commit reloads with no camera frame");
    CHECK(c.fingerprint.resident_count==2 && c.fingerprint.embedding_count==2,
          "no-frame reload candidate contains complete new catalog");
    int64_t b_id=-1;
    for(const auto& [id,resident]:c.resident_by_id) if(resident.ext_id=="B") b_id=id;
    MatchResult b_match=c.matcher.match(unit(8,2),0.5f);
    CHECK(b_id>=0 && b_match.resident_id==b_id,
          "newly reloaded embedding matches without process restart");
    reload.accept(c);CHECK(reload.generation()==2,"no-frame generation accepted");

    // A frame already published at the deadline remains observable.
    {std::lock_guard<std::mutex>lk(slot.mtx);slot.latest=cv::Mat(1,1,CV_8UC3,cv::Scalar(1,2,3));slot.seq++;}
    wr=wait_for_frame_or_deadline(&slot,&stop,&seq,&frame,0.0);
    CHECK(wr==FrameWaitResult::Frame && seq==1 && !frame.empty(),"frame at deadline is preserved");

    ResidentCatalogReloader disabled;
    CHECK(disabled.open(path,8,0,now_ms(),error) && !disabled.enabled(),"interval zero opens no poll reader");
    CHECK(disabled.poll(now_ms(),c,error)==ResidentCatalogReloader::PollResult::NotDue,"disabled reload never polls");

    disabled.close();reload.close();writer.close();
    std::remove(path.c_str());std::remove((path+"-wal").c_str());std::remove((path+"-shm").c_str());
    std::printf("\n[test_reload_no_frame] %d checks, %d failed\n",checks,failed);
    return failed?1:0;
}
