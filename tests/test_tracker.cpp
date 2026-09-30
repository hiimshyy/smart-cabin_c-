#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>

#include "tracker.h"
#include "log/logger.h"

static int checks=0, failed=0;
#define CHECK(c,m) do{++checks;if(!(c)){++failed;std::printf("  FAIL: %s (line %d)\n",m,__LINE__);}}while(0)

static PersonDet person(float x=10,float y=20,float w=100,float h=200){
    PersonDet d; d.x1=x; d.y1=y; d.x2=x+w; d.y2=y+h; d.score=0.9f; return d;
}
static Track* only(Tracker& t){ auto a=t.active_tracks(); return a.size()==1?a[0]:nullptr; }

int main(){
    Tracker t;
    std::vector<PersonDet> ds{person()};
    t.update(ds,0); t.update(ds,1); t.update(ds,2);
    Track* p=only(t);
    CHECK(p!=nullptr,"track confirmed");
    const int id=p?p->id:-1;
    const float x1=p?p->x1:0, y1=p?p->y1:0, x2=p?p->x2:0, y2=p?p->y2:0;

    t.record_recognition(id,"unknown",0.1f,2);
    p=only(t);
    CHECK(p && !t.needs_recog(*p,2),"fresh unknown remains cached before invalidation");
    auto ids=t.invalidate_catalog_cache({},true);
    p=only(t);
    CHECK(ids.size()==1 && ids[0]==id,"unknown track invalidated for added resident");
    CHECK(p && p->id==id && p->x1==x1 && p->y1==y1 && p->x2==x2 && p->y2==y2,
          "invalidation preserves track id and geometry");
    CHECK(p && p->name.empty() && p->match_sim<0 && p->last_recog_frame==-1,
          "invalidation clears recognition fields");
    CHECK(p && t.needs_recog(*p,3),"invalidated confirmed track needs recognition next frame");

    t.record_recognition(id,"known-label",0.8f,3);
    auto none=t.invalidate_catalog_cache({},false);
    p=only(t);
    CHECK(none.empty() && p && p->name=="known-label","unaffected known track preserved");
    auto selected=t.invalidate_catalog_cache(std::set<int>{id},false);
    p=only(t);
    CHECK(selected.size()==1 && p && p->name.empty(),"explicit affected track invalidated");

    // Two-track selectivity and non-recognition state preservation.
    Tracker two;
    std::vector<PersonDet> pair{person(10,20),person(300,20)};
    two.update(pair,0);two.update(pair,1);two.update(pair,2);
    auto ta=two.active_tracks(); CHECK(ta.size()==2,"two-track test confirmed");
    int first=-1,second=-1;
    for(auto* x:ta){if(x->x1<200)first=x->id;else second=x->id;}
    two.record_recognition(first,"known-a",0.8f,2);
    two.record_recognition(second,"known-b",0.8f,2);
    Track before;
    for(const auto& x:two.all_tracks())if(x.id==first)before=x;
    auto one=two.invalidate_catalog_cache(std::set<int>{first},false);
    const Track *after_first=nullptr,*after_second=nullptr;
    for(const auto& x:two.all_tracks()){if(x.id==first)after_first=&x;if(x.id==second)after_second=&x;}
    CHECK(one==std::vector<int>{first},"only explicit affected track returned");
    CHECK(after_first && after_first->state==before.state &&
          after_first->consecutive_hits==before.consecutive_hits &&
          after_first->missed_frames==before.missed_frames &&
          after_first->first_seen_frame==before.first_seen_frame &&
          after_first->last_seen_frame==before.last_seen_frame,
          "invalidation preserves state, age, hits and misses");
    CHECK(after_second && after_second->name=="known-b","unaffected second track stays recognized");

    // Build a known ghost, then prove a catalog content reload purges it.
    Tracker g;
    g.update(ds,0); g.update(ds,1); g.update(ds,2);
    Track* gp=only(g); CHECK(gp!=nullptr,"ghost test track confirmed");
    if(gp) g.record_recognition(gp->id,"private-display-label",0.9f,2);
    std::vector<PersonDet> empty;
    for(int f=3;f<40;++f) g.update(empty,f);
    CHECK(g.ghost_count()==1,"known killed track enters ghost cache");
    g.invalidate_catalog_cache({},false);
    CHECK(g.ghost_count()==0,"catalog reload purges name-only ghost cache");

    // Runtime privacy regression: exercise actual ghost inheritance while
    // capturing the structured log; the private display label must not appear.
    Tracker inherit;
    inherit.update(ds,0);inherit.update(ds,1);inherit.update(ds,2);
    Track* old=only(inherit); if(old)inherit.record_recognition(old->id,"secret-person-label",0.9f,2);
    for(int f=3;f<40;++f)inherit.update(empty,f);
    inherit.update(ds,40);inherit.update(ds,41);inherit.update(ds,42);
    Track* fresh=only(inherit); CHECK(fresh!=nullptr && inherit.ghost_count()==1,"inheritance setup ready");
    FILE* capture=std::tmpfile(); int saved_stderr=dup(fileno(stderr));
    std::fflush(stderr); dup2(fileno(capture),fileno(stderr));
    LogConfig lc; lc.to_stderr=true; lc.to_file=false; lc.color=false; lc.level=LogLevel::INFO;
    Logger::instance().init(lc);
    if(fresh)inherit.record_recognition(fresh->id,"secret-person-label",0.91f,42);
    Logger::instance().flush(); std::fflush(stderr);
    dup2(saved_stderr,fileno(stderr)); close(saved_stderr);
    std::rewind(capture); std::string log; char buf[512];
    while(std::fgets(buf,sizeof(buf),capture))log+=buf;
    std::fclose(capture); Logger::instance().shutdown();
    CHECK(log.find("inherit ghost track_id=")!=std::string::npos,"numeric inheritance log emitted");
    CHECK(log.find("secret-person-label")==std::string::npos,"inheritance log contains no display name");
    fresh=only(inherit);
    CHECK(fresh && fresh->id==1,"same-label re-entry inherits ghost track ID when not purged");

    std::printf("\n[test_tracker] %d checks, %d failed\n",checks,failed);
    return failed?1:0;
}
