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
    std::printf("\n[test_app_config] %d checks, %d failed\n",checks,failed);
    return failed?1:0;
}
