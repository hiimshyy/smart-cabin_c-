#include <cstdio>
#include <vector>

#include "app_config.h"

static int checks=0,failed=0;
#define CHECK(c,m) do{++checks;if(!(c)){++failed;std::printf("  FAIL: %s (line %d)\n",m,__LINE__);}}while(0)

struct Parsed { AppConfig cfg; ParseResult result; };
static Parsed parse(std::vector<const char*> args){
    Parsed p; std::vector<char*> av; for(auto* s:args)av.push_back(const_cast<char*>(s));
    p.result=parse_args((int)av.size(),av.data(),p.cfg); return p;
}
int main(){
    auto d=parse({"app"});
    CHECK(d.cfg.resident_reload_ms==1000 && !d.result.invalid_args,"default reload interval 1000ms");
    CHECK(d.cfg.person_decode_threshold()==d.cfg.person_thr,
          "greedy fallback retains the original high-confidence YOLO threshold");
    auto off=parse({"app","--resident-reload-ms","0"});
    CHECK(off.cfg.resident_reload_ms==0 && !off.result.invalid_args,"zero disables reload");
    auto custom=parse({"app","--resident-reload-ms","250"});
    CHECK(custom.cfg.resident_reload_ms==250 && !custom.result.invalid_args,"custom interval parsed");
    auto negative=parse({"app","--resident-reload-ms","-5"});
    CHECK(negative.result.invalid_args,"negative interval rejected");
    auto text=parse({"app","--resident-reload-ms","abc"});
    CHECK(text.result.invalid_args,"nonnumeric interval rejected");
    auto missing=parse({"app","--resident-reload-ms"});
    CHECK(missing.result.invalid_args,"missing value rejected");
    auto flag=parse({"app","--resident-reload-ms","--resident-db","x.db"});
    CHECK(flag.result.invalid_args && flag.cfg.resident_db_path==nullptr,
          "option token cannot be consumed as reload interval");
    auto byte=parse({"app","--tracker-mode","byte","--person-low-thr","0.2","--person-thr","0.6",
                     "--track-lost-ms","1000"});
    CHECK(!byte.result.invalid_args && byte.cfg.tracker_mode==TrackerMode::Byte &&
          byte.cfg.person_low_thr==0.2f && byte.cfg.track_lost_ms==1000,
          "ByteTrack CLI configuration parsed");
    CHECK(byte.cfg.person_decode_threshold()==byte.cfg.person_low_thr,
          "ByteTrack decodes low-confidence detections for recovery");
    auto bad_thr=parse({"app","--person-low-thr","0.6","--person-thr","0.5"});
    CHECK(bad_thr.result.invalid_args,"inverted person thresholds rejected");
    auto bad_mode=parse({"app","--tracker-mode","sort"});
    CHECK(bad_mode.result.invalid_args,"invalid tracker mode rejected");
    auto record=parse({"app","--person-model","person.nb","--record-dir","clips",
                       "--record-pre-ms","1500","--record-empty-ms","5000",
                       "--record-fps","12.5","--record-width","1280"});
    CHECK(!record.result.invalid_args && record.cfg.record_dir &&
          std::string(record.cfg.record_dir)=="clips" && record.cfg.record_pre_ms==1500 &&
          record.cfg.record_empty_ms==5000 && record.cfg.record_fps==12.5 &&
          record.cfg.record_width==1280,
          "person-triggered recording options parsed");
    auto record_no_person=parse({"app","--record-dir","clips"});
    CHECK(record_no_person.result.invalid_args,"recording requires person detector");
    auto record_bad_empty=parse({"app","--person-model","person.nb","--record-dir","clips",
                                 "--record-empty-ms","0"});
    CHECK(record_bad_empty.result.invalid_args,"record empty timeout must be positive");
    auto record_bad_fps=parse({"app","--person-model","person.nb","--record-dir","clips",
                               "--record-fps","fast"});
    CHECK(record_bad_fps.result.invalid_args,"record fps must be numeric");
    auto record_bad_width=parse({"app","--person-model","person.nb","--record-dir","clips",
                                 "--record-width","-1"});
    CHECK(record_bad_width.result.invalid_args,"record width must be zero or positive");
    std::printf("\n[test_app_config] %d checks, %d failed\n",checks,failed);
    return failed?1:0;
}
