#include <cstdio>
#include <map>
#include <vector>

#include "resident_reconcile.h"

static int checks=0, failed=0;
#define CHECK(c,m) do{++checks;if(!(c)){++failed;std::printf("  FAIL: %s (line %d)\n",m,__LINE__);}}while(0)

static PersonDet det(float x){ PersonDet d{x,10,x+80,210,0.9f}; return d; }
static std::vector<std::pair<int,MatchResult>> subjects(int a_key,int64_t a_rid,
                                                       int b_key,int64_t b_rid){
    MatchResult a; a.resident_id=a_rid; a.similarity=0.9f;
    MatchResult b; b.resident_id=b_rid; b.similarity=0.1f;
    return {{a_key,a},{b_key,b}};
}

int main(){
    InteractionConfig cfg; cfg.confirm_streak=1; cfg.cooldown_ms=1000;
    cfg.unknown_after_ms=5000; cfg.reap_grace_ms=5000;

    Tracker tracker;
    std::vector<PersonDet> ds{det(10),det(200)};
    tracker.update(ds,0);tracker.update(ds,1);tracker.update(ds,2);
    auto tracks=tracker.active_tracks();
    CHECK(tracks.size()==2,"two tracks confirmed");
    int known_id=-1,unknown_id=-1;
    for(auto* t:tracks){ if(t->x1<100)known_id=t->id;else unknown_id=t->id; }
    tracker.record_recognition(known_id,"known",0.9f,2);
    tracker.record_recognition(unknown_id,"unknown",0.1f,2);

    std::map<int,int64_t> mapping{{known_id,10}};
    InteractionManager interaction(cfg);
    interaction.update(subjects(known_id,10,unknown_id,-1),0.0);
    CHECK(interaction.session_count()==2,"known and unknown sessions exist");

    CatalogDiff add; add.added.insert(20);
    auto ar=reconcile_catalog_runtime(add,true,tracker,mapping,interaction);
    CHECK(ar.affected_subject_keys.size()==1 && ar.affected_subject_keys[0]==unknown_id,
          "added resident invalidates only unknown track");
    CHECK(mapping.size()==1 && mapping.count(known_id),"unrelated known mapping preserved");
    CHECK(interaction.session_count()==1,"only unknown subject session dropped");
    Track* known=nullptr; Track* unknown=nullptr;
    for(auto* t:tracker.active_tracks()){if(t->id==known_id)known=t;if(t->id==unknown_id)unknown=t;}
    CHECK(known && known->name=="known","unrelated known tracker cache preserved");
    CHECK(unknown && unknown->name.empty() && tracker.needs_recog(*unknown,3),
          "unknown track retries on next frame");

    CatalogDiff remove; remove.removed.insert(10);
    auto rr=reconcile_catalog_runtime(remove,true,tracker,mapping,interaction);
    CHECK(rr.affected_subject_keys.size()==1 && rr.affected_subject_keys[0]==known_id,
          "removed resident invalidates mapped track");
    CHECK(mapping.count(known_id)==0 && rr.erased_track_mappings==1,
          "removed resident mapping erased atomically");
    CHECK(interaction.session_count()==0,"removed resident session dropped");

    // Simulate main's cached fallback after reload. With mapping erased, an
    // unknown result must stay unknown and cannot rebuild an old confirmation.
    MatchResult now_unknown; now_unknown.resident_id=-1; now_unknown.similarity=0.1f;
    auto mi=mapping.find(known_id);
    if(mi!=mapping.end()) now_unknown.resident_id=mi->second;
    auto outcomes=interaction.update({{known_id,now_unknown}},2000.0);
    bool confirmed=false; for(const auto& o:outcomes)confirmed|=o.confirmed;
    CHECK(!confirmed && now_unknown.resident_id<0,"stale fallback cannot resurrect removed resident");

    // A stale mapping/session can outlive the actual Track. It must still be
    // removed based on the mapping's resident ID, not Tracker's return list.
    Tracker no_live_tracks;
    std::map<int,int64_t> stale_mapping{{999,10}};
    InteractionManager stale_interaction(cfg);
    MatchResult stale_match; stale_match.resident_id=10; stale_match.similarity=0.9f;
    stale_interaction.update({{999,stale_match}},0.0);
    auto stale=reconcile_catalog_runtime(remove,true,no_live_tracks,stale_mapping,stale_interaction);
    CHECK(stale.invalidated_live_tracks==0,"stale subject has no live tracker entry");
    CHECK(stale.affected_subject_keys==std::vector<int>{999},"stale mapped subject still reconciled");
    CHECK(stale_mapping.empty() && stale_interaction.session_count()==0,
          "stale mapping and session removed without live track");

    // SCRFD-only key 0 recognizes every frame. Adding unrelated B must retain
    // confirmed A even after cooldown; changing A itself drops key 0.
    Tracker unused;
    std::map<int,int64_t> no_map;
    InteractionManager face_only(cfg);
    MatchResult a; a.resident_id=10; a.similarity=0.9f;
    face_only.update({{0,a}},0.0); // confirmed immediately
    auto fo_add=reconcile_catalog_runtime(add,false,unused,no_map,face_only);
    CHECK(fo_add.affected_subject_keys.empty() && face_only.session_count()==1,
          "SCRFD-only unrelated add preserves key0 session");
    auto no_duplicate=face_only.update({{0,a}},2000.0);
    CHECK(no_duplicate.empty(),"SCRFD-only add does not duplicate confirm after cooldown");
    CatalogDiff change; change.changed.insert(10);
    auto fo_change=reconcile_catalog_runtime(change,false,unused,no_map,face_only);
    CHECK(fo_change.affected_subject_keys==std::vector<int>{0} && face_only.session_count()==0,
          "SCRFD-only changed current resident reconciles key0");

    std::printf("\n[test_resident_reconcile] %d checks, %d failed\n",checks,failed);
    return failed?1:0;
}
