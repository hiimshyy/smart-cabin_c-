#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

#include "resident_catalog.h"
#include "resident_enroll.h"

static int checks = 0, failed = 0;
#define CHECK(c,m) do { ++checks; if (!(c)) { ++failed; std::printf("  FAIL: %s (line %d)\n",m,__LINE__); } } while(0)

static std::vector<float> unit_vec(int dim, float seed) {
    std::vector<float> v(dim); double n=0;
    for (int i=0;i<dim;++i) { v[i]=seed+0.013f*i; n+=(double)v[i]*v[i]; }
    n=std::sqrt(n); for(float& x:v)x=(float)(x/n); return v;
}
static ResidentEnrollRequest req(const char* ext, float seed) {
    ResidentEnrollRequest r; r.ext_id=ext; r.name=ext; r.home_floor=3; r.role="staff";
    r.replace=true; r.embeddings={unit_vec(8,seed)}; return r;
}
static bool raw_exec(const std::string& path, const std::string& sql) {
    sqlite3* db=nullptr; if(sqlite3_open(path.c_str(),&db)!=SQLITE_OK)return false;
    char* err=nullptr; int rc=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,&err);
    if(err)sqlite3_free(err); sqlite3_close(db); return rc==SQLITE_OK;
}
static long long raw_ll(const std::string& path, const char* sql) {
    sqlite3* db=nullptr; if(sqlite3_open(path.c_str(),&db)!=SQLITE_OK)return -1;
    sqlite3_stmt* st=nullptr; long long v=-1;
    if(sqlite3_prepare_v2(db,sql,-1,&st,nullptr)==SQLITE_OK && sqlite3_step(st)==SQLITE_ROW)
        v=sqlite3_column_int64(st,0);
    if(st)sqlite3_finalize(st); sqlite3_close(db); return v;
}
struct HookCtx { ResidentDB* writer; bool called=false; };
struct AsyncHookCtx { ResidentDB* db; };
static bool async_writer_hook(void* p) {
    auto* c=static_cast<AsyncHookCtx*>(p);
    MatchEvent ev; ev.cabin_id=1; ev.action="unknown";
    c->db->log_event(ev);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    return true;
}
struct FailOnceCtx { int calls=0; };
static bool fail_once_hook(void* p) { return ++static_cast<FailOnceCtx*>(p)->calls > 1; }
static bool snapshot_hook(void* p) {
    auto* c=static_cast<HookCtx*>(p); c->called=true;
    auto result=write_resident_enrollment(*c->writer, req("B",2.0f));
    if(!result.ok) std::printf("  hook enrollment failed: %s\n",result.error.c_str());
    return result.ok;
}

static EmbeddingRow row(int64_t id,int64_t owner,std::vector<float> v) {
    EmbeddingRow e; e.id=id; e.resident_id=owner; e.source="id_photo";
    e.declared_dim=(int)v.size(); e.blob_nbytes=(int)v.size()*4;
    e.metadata_valid=true; e.blob_valid=true;
    e.vector=std::move(v); return e;
}

int main(int argc,char**argv){
    const std::string schema=argc>1?argv[1]:"db/schema.sql";
    const std::string path="/tmp/test_resident_catalog.db";
    std::remove(path.c_str());std::remove((path+"-wal").c_str());std::remove((path+"-shm").c_str());

    ResidentDB writer;
    CHECK(writer.open(path,schema,false),"open writer");
    CHECK(write_resident_enrollment(writer,req("A",1.0f)).ok,"seed A");

    ResidentDB reader;
    CHECK(reader.open_readonly(path,50),"open read-only reader");
    int64_t dv0=-1,dv1=-1;
    CHECK(reader.data_version(dv0),"read initial data_version");

    // Deterministic interleave: B commits after resident SELECT, but the
    // embedding SELECT must stay on the old snapshot (A only).
    HookCtx hc{&writer}; reader.set_snapshot_test_hook(snapshot_hook,&hc);
    std::vector<Resident> rs; std::vector<EmbeddingRow> es;
    CHECK(reader.load_active_snapshot(rs,es),"snapshot with interleaved commit");
    CHECK(hc.called,"snapshot phase hook called");
    CHECK(rs.size()==1 && es.size()==1,"snapshot sees old catalog atomically");
    reader.set_snapshot_test_hook(nullptr,nullptr);
    CHECK(reader.data_version(dv1) && dv1!=dv0,"external commit changes data_version");
    CHECK(reader.load_active_snapshot(rs,es) && rs.size()==2 && es.size()==2,
          "next snapshot sees full committed catalog");

    // Fingerprint is deterministic regardless of caller row order.
    ResidentCatalogCandidate c1,c2; std::string err;
    CHECK(build_resident_catalog(rs,es,8,c1,err),"build valid catalog");
    std::reverse(rs.begin(),rs.end()); std::reverse(es.begin(),es.end());
    CHECK(build_resident_catalog(rs,es,8,c2,err),"build reversed catalog");
    CHECK(c1.fingerprint==c2.fingerprint,"fingerprint independent of row order");

    std::map<int64_t,ResidentDigest> d_old{{1,{10,20}},{2,{30,40}}};
    std::map<int64_t,ResidentDigest> d_new{{1,{10,21}},{3,{50,60}}};
    CatalogDiff dd=diff_resident_catalogs(d_old,d_new);
    CHECK(dd.changed==std::set<int64_t>{1} && dd.removed==std::set<int64_t>{2} &&
          dd.added==std::set<int64_t>{3}, "diff reports changed/removed/added");

    // Strict corruption checks (direct pure-builder tests).
    std::vector<Resident> one={c1.resident_by_id.begin()->second};
    const int64_t owner=one[0].id;
    auto good=row(100,owner,unit_vec(8,3.0f));
    ResidentCatalogCandidate bad;
    auto corrupt=good; corrupt.blob_nbytes+=1;
    CHECK(!build_resident_catalog(one,{corrupt},8,bad,err) && err=="embedding_blob_size_mismatch",
          "reject trailing blob byte");
    corrupt=good; corrupt.vector[0]=NAN;
    CHECK(!build_resident_catalog(one,{corrupt},8,bad,err) && err=="embedding_non_finite",
          "reject NaN");
    corrupt=good; corrupt.vector[0]=INFINITY;
    CHECK(!build_resident_catalog(one,{corrupt},8,bad,err) && err=="embedding_non_finite",
          "reject Inf");
    corrupt=good; corrupt.blob_valid=false; corrupt.metadata_valid=false;
    CHECK(!build_resident_catalog(one,{corrupt},8,bad,err) && err=="embedding_blob_null",
          "reject NULL blob");
    corrupt=good; corrupt.vector.pop_back();
    CHECK(!build_resident_catalog(one,{corrupt},8,bad,err) && err=="embedding_vector_size_mismatch",
          "reject decoded vector-size mismatch");
    corrupt=good; std::fill(corrupt.vector.begin(),corrupt.vector.end(),0.0f);
    CHECK(!build_resident_catalog(one,{corrupt},8,bad,err) && err=="embedding_zero_norm",
          "reject zero vector");
    corrupt=good; for(float& x:corrupt.vector)x*=2.0f;
    CHECK(!build_resident_catalog(one,{corrupt},8,bad,err) && err=="embedding_not_normalized",
          "reject non-normalized vector");

    ResidentCatalogCandidate id_a,id_b;
    auto same_content_new_id=good; same_content_new_id.id=999;
    CHECK(build_resident_catalog(one,{good},8,id_a,err) &&
          build_resident_catalog(one,{same_content_new_id},8,id_b,err) &&
          id_a.fingerprint==id_b.fingerprint,
          "storage row-id churn does not change runtime fingerprint");

    // SQLite INTEGER is 64-bit: a value 2^32+8 must never narrow to 8 or
    // trigger a huge allocation. Loader preserves it and marks metadata bad.
    char overflow_sql[512]; std::snprintf(overflow_sql,sizeof(overflow_sql),
      "INSERT INTO embeddings(resident_id,source,dim,vector) VALUES(%lld,'admin',4294967304,X'0000000000000000000000000000000000000000000000000000000000000000');",
      (long long)owner);
    CHECK(raw_exec(path,overflow_sql),"insert 64-bit overflow dim row");
    std::snprintf(overflow_sql,sizeof(overflow_sql),
      "INSERT INTO embeddings(resident_id,source,dim,vector) VALUES(%lld,'admin',2097152,zeroblob(8388608));",
      (long long)owner);
    CHECK(raw_exec(path,overflow_sql),"insert 8MiB oversized blob row");
    ResidentDB overflow_reader;
    CHECK(overflow_reader.open_readonly(path,50),"open overflow reader");
    std::vector<Resident> ors; std::vector<EmbeddingRow> oes;
    CHECK(overflow_reader.load_active_snapshot(ors,oes),"load overflow metadata safely");
    bool saw_overflow=false, saw_oversized=false;
    for(const auto& e:oes) {
        if(e.declared_dim==4294967304LL) {
            saw_overflow=true; CHECK(!e.metadata_valid && e.vector.empty(),
                                    "overflow dim rejected before allocation");
        }
        if(e.blob_nbytes==8388608) {
            saw_oversized=true; CHECK(!e.metadata_valid && e.vector.empty(),
                                     "oversized blob payload not materialized into vector");
        }
    }
    CHECK(saw_overflow,"64-bit declared dim preserved exactly");
    CHECK(saw_oversized,"oversized blob metadata observed safely");
    overflow_reader.close();
    CHECK(raw_exec(path,"DELETE FROM embeddings WHERE dim IN (4294967304,2097152);"),
          "remove overflow/oversized rows");

    reader.close();

    // Reloader state machine: initial content, audit-only no-op, then add.
    ResidentCatalogReloader reload;
    CHECK(reload.open(path,8,1000,0,err),"open reloader");
    ResidentCatalogCandidate out;
    CHECK(reload.poll(0,out,err)==ResidentCatalogReloader::PollResult::Reloaded,
          "initial forced snapshot reloads");
    reload.accept(out);
    CHECK(reload.generation()==1,"generation 1 accepted");
    CHECK(reload.stats().matcher_builds==1,"initial content builds matcher once");
    CHECK(reload.poll(500,out,err)==ResidentCatalogReloader::PollResult::NotDue,"interval enforced");

    CHECK(raw_exec(path,"INSERT INTO match_events(cabin_id,action) VALUES(1,'unknown');"
                        "UPDATE residents SET match_count=match_count+1,last_seen_at=CURRENT_TIMESTAMP WHERE ext_id='A';"),
          "write audit-only data");
    CHECK(reload.poll(1000,out,err)==ResidentCatalogReloader::PollResult::NoCatalogChange,
          "audit/touch data does not reload catalog");
    CHECK(reload.generation()==1,"audit keeps generation");
    CHECK(reload.stats().matcher_builds==1,"audit-only commit does not rebuild matcher");
    CHECK(reload.stats().version_changes==1 && reload.stats().full_scans==2,
          "stats distinguish one real version change from initial full scan");

    CHECK(write_resident_enrollment(writer,req("C",4.0f)).ok,"external add C");
    CHECK(reload.poll(2000,out,err)==ResidentCatalogReloader::PollResult::Reloaded,
          "external resident add reloads");
    CHECK(out.diff.added.size()==1,"diff reports one added resident");
    reload.accept(out);
    CHECK(reload.generation()==2 && out.matcher.vector_count()==3,"generation 2 has full catalog");
    CHECK(reload.stats().matcher_builds==2,"real content change builds matcher");
    CHECK(reload.stats().version_changes==2,"stats count genuine second version change");

    // A transient snapshot failure must retry on the next interval even though
    // no additional commit/data_version change occurs.
    CHECK(raw_exec(path,"UPDATE residents SET role='manager',updated_at=CURRENT_TIMESTAMP WHERE ext_id='C';"),
          "commit metadata change for retry test");
    FailOnceCtx fc;
    reload.set_reader_snapshot_hook_for_test(fail_once_hook,&fc);
    CHECK(reload.poll(3000,out,err)==ResidentCatalogReloader::PollResult::Failed,
          "transient snapshot failure reported");
    CHECK(reload.generation()==2,"transient failure keeps active generation");
    CHECK(reload.poll(4000,out,err)==ResidentCatalogReloader::PollResult::Reloaded,
          "same data_version retried successfully");
    reload.set_reader_snapshot_hook_for_test(nullptr,nullptr);
    reload.accept(out);
    CHECK(reload.generation()==3,"retried candidate accepted");

    // Corrupt row: reject candidate and retain active generation. Remove it,
    // then polling must recover without losing the accepted catalog.
    const int64_t rid=c1.resident_by_id.begin()->first;
    char sql[512]; std::snprintf(sql,sizeof(sql),
      "INSERT INTO embeddings(resident_id,source,dim,vector) VALUES(%lld,'admin',8,X'000000000000000000000000000000000000000000000000000000000000000000');",
      (long long)rid); // 33 bytes for declared 8 floats
    CHECK(raw_exec(path,sql),"insert malformed trailing-byte blob");
    CHECK(reload.poll(5000,out,err)==ResidentCatalogReloader::PollResult::Failed,
          "malformed candidate rejected");
    CHECK(reload.generation()==3,"failed candidate preserves active generation");
    CHECK(raw_exec(path,"DELETE FROM embeddings WHERE length(vector)=33;"),"remove malformed row");
    auto recovered=reload.poll(6000,out,err);
    CHECK(recovered==ResidentCatalogReloader::PollResult::NoCatalogChange,
          "retry recovers to unchanged active catalog");

    // Reconnect must force a full snapshot even if connection-local version
    // happens to have the same numeric value.
    reload.disconnect_reader_for_test();
    auto rr=reload.poll(7000,out,err);
    CHECK(rr==ResidentCatalogReloader::PollResult::NoCatalogChange,
          "reader reconnect forces and validates full snapshot");

    reload.close(); writer.close();

    // DELETE-journal exclusive lock deterministically blocks a reader. Verify
    // the configured 50ms timeout returns control promptly and the same reader
    // connection remains usable after the lock is released.
    CHECK(raw_exec(path,"PRAGMA journal_mode=DELETE;"),"switch temp DB to delete journal");
    sqlite3* locker=nullptr;
    CHECK(sqlite3_open(path.c_str(),&locker)==SQLITE_OK,"open exclusive locker");
    CHECK(sqlite3_exec(locker,"BEGIN EXCLUSIVE;",nullptr,nullptr,nullptr)==SQLITE_OK,
          "acquire exclusive DB lock");
    ResidentDB blocked_reader;
    CHECK(blocked_reader.open_readonly(path,50),"open reader under exclusive lock");
    auto bt0=std::chrono::steady_clock::now();
    bool blocked_ok=blocked_reader.load_active_snapshot(rs,es);
    auto blocked_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-bt0).count();
    CHECK(!blocked_ok,"snapshot reports busy under exclusive lock");
    CHECK(blocked_ms < 500,"busy timeout returns promptly, not 5 seconds");
    sqlite3_exec(locker,"ROLLBACK;",nullptr,nullptr,nullptr); sqlite3_close(locker);
    CHECK(blocked_reader.load_active_snapshot(rs,es),"reader reusable after busy lock release");
    blocked_reader.close();

    // Public load_active_snapshot remains safe even on the primary connection
    // with its async writer: db_mtx serializes the writer batch until snapshot
    // COMMIT instead of nesting transactions on one sqlite3 handle.
    const std::string async_path="/tmp/test_resident_catalog_async.db";
    std::remove(async_path.c_str()); std::remove((async_path+"-wal").c_str());
    std::remove((async_path+"-shm").c_str());
    ResidentDB async_db;
    CHECK(async_db.open(async_path,schema,true),"open writer-enabled DB");
    CHECK(write_resident_enrollment(async_db,req("ASYNC",5.0f)).ok,"seed async DB");
    AsyncHookCtx ac{&async_db}; async_db.set_snapshot_test_hook(async_writer_hook,&ac);
    CHECK(async_db.load_active_snapshot(rs,es),"snapshot serialized with async writer");
    async_db.set_snapshot_test_hook(nullptr,nullptr);
    async_db.close();
    CHECK(raw_ll(async_path,"SELECT COUNT(*) FROM match_events;")==1,
          "queued event flushes after snapshot without transaction collision");
    std::remove(async_path.c_str()); std::remove((async_path+"-wal").c_str());
    std::remove((async_path+"-shm").c_str());

    std::remove(path.c_str());std::remove((path+"-wal").c_str());std::remove((path+"-shm").c_str());
    std::printf("\n[test_resident_catalog] %d checks, %d failed\n",checks,failed);
    return failed?1:0;
}
