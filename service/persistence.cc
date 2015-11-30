//
// Created by wastl on 15.11.15.
//
#define KEY_LENGTH 16

#include <chrono>

#include <glog/logging.h>
#include <leveldb/filter_policy.h>
#include <leveldb/write_batch.h>

#include "persistence.h"
#include "model/rdf_operators.h"

#define CHECK_STATUS(s) CHECK(s.ok()) << "Writing to database failed: " << s.ToString()

#if KEY_LENGTH == 16
#include "util/murmur3.h"
#endif

using leveldb::WriteBatch;
using marmotta::rdf::proto::Statement;
using marmotta::rdf::proto::Namespace;
using marmotta::rdf::proto::Resource;

namespace marmotta {
namespace persistence {

#if KEY_LENGTH == 8
static std::hash<std::string> g_hash_fn;

/**
* Encode a 64bit integer in the first 8 bytes of the buffer.
*/
void encodeInt(char* buffer, size_t data) {
    for (int i=0; i<KEY_LENGTH; i++) {
        buffer[i] = (char)((data >> ((KEY_LENGTH-i-1)*8)) & 0xFF);
    }
}
#endif

// Creates an index key based on hashing values of the 4 messages in proper order.
void computeKey(const std::string* a, const std::string* b, const std::string* c, const std::string* d, char* result) {
#if KEY_LENGTH == 16
    // 128bit keys, use murmur
    int offset = 0;
    for (auto m : {a, b, c, d}) {
        if (m != nullptr) {
            MurmurHash3_x64_128(m->data(), m->size(), 13, &result[offset]);
        } else {
            return;
        }
        offset += KEY_LENGTH;
    }

#else
    // 64bit keys
    int offset = 0;
    for (auto m : {a, b, c, d}) {
        if (m != nullptr) {
            size_t h = g_hash_fn(*m);
            encodeInt(&result[offset], h);
        } else {
            return;
        }
        offset += KEY_LENGTH;
    }
#endif
}

/**
 * Helper class to define proper cache keys and identify the index to use based on
 * fields available in the pattern.
 */
class PatternQuery {
 public:
    enum IndexType {
        SPOC, CSPO, OPSC, PCOS
    };

    PatternQuery(const Statement& pattern) : pattern(pattern) {
        if (pattern.has_subject()) {
            s.reset(new std::string());
            pattern.subject().SerializeToString(s.get());
        }
        if (pattern.has_predicate()) {
            p.reset(new std::string());
            pattern.predicate().SerializeToString(p.get());
        }
        if (pattern.has_object()) {
            o.reset(new std::string());
            pattern.object().SerializeToString(o.get());
        }
        if (pattern.has_context()) {
            c.reset(new std::string());
            pattern.context().SerializeToString(c.get());
        }

        if (pattern.has_subject()) {
            // Subject is usually most selective, so if it is present use the
            // subject-based databases first.
            if (pattern.has_context()) {
                type_ = CSPO;
            } else {
                type_ = SPOC;
            }
        } else if (pattern.has_object()) {
            // Second-best option is object.
            type_ = OPSC;
        } else if (pattern.has_predicate()) {
            // Predicate is usually least selective.
            type_ = PCOS;
        } else {
            // Fall back to SPOC.
            type_ = SPOC;
        }
    }

    /**
     * Return the lower key for querying the index (range [MinKey,MaxKey) ).
     */
    char* MinKey() const {
        char* result = (char*)calloc(4 * KEY_LENGTH, sizeof(char));
        compute(result);
        return result;
    }

    /**
     * Return the upper key for querying the index (range [MinKey,MaxKey) ).
     */
    char* MaxKey() const {
        char* result = (char*)malloc(4 * KEY_LENGTH * sizeof(char));
        for (int i=0; i < 4 * KEY_LENGTH; i++) {
            result[i] = (char)0xFF;
        }

        compute(result);
        return result;
    }

    IndexType Type() const {
        return type_;
    }

    PatternQuery& Type(IndexType t) {
        type_ = t;
        return *this;
    }

 private:
    const Statement& pattern;
    std::unique_ptr<std::string> s, p, o, c;

    // Creates a cache key based on hashing values of the 4 messages in proper order.
    void compute(char* result) const {
        switch(Type()) {
            case SPOC:
                computeKey(s.get(), p.get(), o.get(), c.get(), result);
                break;
            case CSPO:
                computeKey(c.get(), s.get(), p.get(), o.get(), result);
                break;
            case OPSC:
                computeKey(o.get(), p.get(), s.get(), c.get(), result);
                break;
            case PCOS:
                computeKey(p.get(), c.get(), o.get(), s.get(), result);
                break;
        }
    }

    IndexType type_;
};

/**
 * Check if a statement matches with a partial pattern.
 */
bool matches(const Statement& stmt, const Statement& pattern) {
    // equality operators defined in rdf_model.h
    if (pattern.has_context() && stmt.context() != pattern.context()) {
        return false;
    }
    if (pattern.has_subject() && stmt.subject() != pattern.subject()) {
        return false;
    }
    if (pattern.has_predicate() && stmt.predicate() != pattern.predicate()) {
        return false;
    }
    if (pattern.has_object() && stmt.object() != pattern.object()) {
        return false;
    }
    return true;
}


/**
 * Build database with default options.
 */
leveldb::DB* buildDB(const std::string& path, const std::string& suffix, const leveldb::Options& options) {
    leveldb::DB* db;
    leveldb::Status status = leveldb::DB::Open(options, path + "_" + suffix + ".db", &db);
    assert(status.ok());
    return db;
}

leveldb::Options* buildOptions(KeyComparator* cmp, leveldb::Cache* cache) {
    leveldb::Options *options = new leveldb::Options();
    options->create_if_missing = true;

    // Custom comparator for our keys.
    options->comparator = cmp;

    // Cache reads in memory.
    options->block_cache = cache;

    // Set a bloom filter of 10 bits.
    options->filter_policy = leveldb::NewBloomFilterPolicy(10);
    return options;
}

leveldb::Options buildNsOptions() {
    leveldb::Options options;
    options.create_if_missing = true;
    return options;
}

LevelDBPersistence::LevelDBPersistence(const std::string &path, int64_t cacheSize)
        : comparator(new KeyComparator())
        , cache(leveldb::NewLRUCache(cacheSize))
        , options(buildOptions(comparator.get(), cache.get()))
        , db_spoc(buildDB(path, "spoc", *options)), db_cspo(buildDB(path, "cspo", *options))
        , db_opsc(buildDB(path, "opsc", *options)), db_pcos(buildDB(path, "pcos", *options))
        , db_ns_prefix(buildDB(path, "ns_prefix", buildNsOptions()))
        , db_ns_url(buildDB(path, "ns_url", buildNsOptions())) { }


int64_t LevelDBPersistence::AddNamespaces(NamespaceIterator& begin, const NamespaceIterator& end) {
    DLOG(INFO) << "Starting batch namespace import operation.";
    int64_t count = 0;

    leveldb::WriteBatch batch_prefix, batch_url;

    for (auto& it = begin; begin != end; ++it) {
        AddNamespace(*it, batch_prefix, batch_url);
        count++;
    }
    CHECK_STATUS(db_ns_prefix->Write(leveldb::WriteOptions(), &batch_prefix));
    CHECK_STATUS(db_ns_url->Write(leveldb::WriteOptions(), &batch_url));

    DLOG(INFO) << "Imported " << count << " namespaces";

    return count;
}

void LevelDBPersistence::GetNamespaces(
        const Namespace &pattern, LevelDBPersistence::NamespaceHandler callback) {
    DLOG(INFO) << "Get namespaces matching pattern " << pattern.DebugString();
    int64_t count = 0;

    Namespace ns;

    leveldb::DB *db = nullptr;
    std::string key, value;
    if (pattern.prefix() != "") {
        key = pattern.prefix();
        db = db_ns_prefix.get();
    } else if(pattern.uri() != "") {
        key = pattern.uri();
        db = db_ns_url.get();
    }
    if (db != nullptr) {
        // Either prefix or uri given, report the correct namespace value.
        leveldb::Status s = db->Get(leveldb::ReadOptions(), key, &value);
        if (s.ok()) {
            ns.ParseFromString(value);
            callback(ns);
            count++;
        }
    } else {
        // Pattern was empty, iterate over all namespaces and report them.
        std::unique_ptr<leveldb::Iterator> it(db_ns_prefix->NewIterator(leveldb::ReadOptions()));
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            ns.ParseFromArray(it->value().data(), it->value().size());
            callback(ns);
            count++;
        }
    }
    DLOG(INFO) << "Get namespaces done (count=" << count <<")";
}


int64_t LevelDBPersistence::AddStatements(StatementIterator& begin, const StatementIterator& end) {
    auto start = std::chrono::steady_clock::now();
    LOG(INFO) << "Starting batch statement import operation.";
    int64_t count = 0;

    leveldb::WriteBatch batch_spoc, batch_cspo, batch_opsc, batch_pcos;
    for (auto& it = begin; begin != end; ++it) {
        AddStatement(*it, batch_spoc, batch_cspo, batch_opsc, batch_pcos);
        count++;
    }
    CHECK_STATUS(db_pcos->Write(leveldb::WriteOptions(), &batch_pcos));
    CHECK_STATUS(db_opsc->Write(leveldb::WriteOptions(), &batch_opsc));
    CHECK_STATUS(db_cspo->Write(leveldb::WriteOptions(), &batch_cspo));
    CHECK_STATUS(db_spoc->Write(leveldb::WriteOptions(), &batch_spoc));

    LOG(INFO) << "Imported " << count << " statements (time="
              << std::chrono::duration <double, std::milli> (
                   std::chrono::steady_clock::now() - start).count()
              << "ms).";

    return count;
}



void LevelDBPersistence::GetStatements(
        const Statement& pattern, std::function<void(const Statement&)> callback) {
    auto start = std::chrono::steady_clock::now();
    DLOG(INFO) << "Get statements matching pattern " << pattern.DebugString();
    int64_t count = 0;

    PatternQuery query(pattern);

    leveldb::DB* db;
    char *loKey = query.MinKey();
    char *hiKey = query.MaxKey();

    switch (query.Type()) {
        case PatternQuery::SPOC:
            db = db_spoc.get();
            DLOG(INFO) << "Query: Using index type SPOC";
            break;
        case PatternQuery::CSPO:
            db = db_cspo.get();
            DLOG(INFO) << "Query: Using index type CSPO";
            break;
        case PatternQuery::OPSC:
            db = db_opsc.get();
            DLOG(INFO) << "Query: Using index type OPSC";
            break;
        case PatternQuery::PCOS:
            db = db_pcos.get();
            DLOG(INFO) << "Query: Using index type PCOS";
            break;
    };

    Statement stmt;
    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    for (it->Seek(leveldb::Slice(loKey, 4 * KEY_LENGTH));
         it->Valid() && it->key().compare(leveldb::Slice(hiKey, 4 * KEY_LENGTH)) <= 0;
         it->Next()) {
        stmt.ParseFromString(it->value().ToString());
        if (matches(stmt, pattern)) {
            callback(stmt);
            count++;
        }
    }

    delete it;
    free(loKey);
    free(hiKey);

    DLOG(INFO) << "Get statements done (count=" << count << ", time="
               << std::chrono::duration <double, std::milli> (
                    std::chrono::steady_clock::now() - start).count()
               << "ms).";
}


int64_t LevelDBPersistence::RemoveStatements(const rdf::proto::Statement& pattern) {
    auto start = std::chrono::steady_clock::now();
    DLOG(INFO) << "Remove statements matching pattern " << pattern.DebugString();

    int64_t count = 0;

    Statement stmt;
    leveldb::WriteBatch batch_spoc, batch_cspo, batch_opsc, batch_pcos;

    count = RemoveStatements(pattern, batch_spoc, batch_cspo, batch_opsc, batch_pcos);

    CHECK_STATUS(db_pcos->Write(leveldb::WriteOptions(), &batch_pcos));
    CHECK_STATUS(db_opsc->Write(leveldb::WriteOptions(), &batch_opsc));
    CHECK_STATUS(db_cspo->Write(leveldb::WriteOptions(), &batch_cspo));
    CHECK_STATUS(db_spoc->Write(leveldb::WriteOptions(), &batch_spoc));

    DLOG(INFO) << "Removed " << count << " statements (time=" <<
               std::chrono::duration <double, std::milli> (
                       std::chrono::steady_clock::now() - start).count()
               << "ms).";

    return count;
}

UpdateStatistics LevelDBPersistence::Update(LevelDBPersistence::UpdateIterator &begin,
                                            const LevelDBPersistence::UpdateIterator &end) {
    auto start = std::chrono::steady_clock::now();
    DLOG(INFO) << "Starting batch update operation.";
    UpdateStatistics stats;

    WriteBatch b_spoc, b_cspo, b_opsc, b_pcos, b_prefix, b_url;
    for (auto& it = begin; begin != end; ++it) {
        if (it->has_stmt_added()) {
            AddStatement(it->stmt_added(), b_spoc, b_cspo, b_opsc, b_pcos);
            stats.added_stmts++;
        } else if (it->has_stmt_removed()) {
            stats.removed_stmts +=
                    RemoveStatements(it->stmt_removed(), b_spoc, b_cspo, b_opsc, b_pcos);
        } else if(it->has_ns_added()) {
            AddNamespace(it->ns_added(), b_prefix, b_url);
            stats.added_ns++;
        } else if(it->has_ns_removed()) {
            RemoveNamespace(it->ns_removed(), b_prefix, b_url);
        }
    }
    CHECK_STATUS(db_pcos->Write(leveldb::WriteOptions(), &b_pcos));
    CHECK_STATUS(db_opsc->Write(leveldb::WriteOptions(), &b_opsc));
    CHECK_STATUS(db_cspo->Write(leveldb::WriteOptions(), &b_cspo));
    CHECK_STATUS(db_spoc->Write(leveldb::WriteOptions(), &b_spoc));
    CHECK_STATUS(db_ns_prefix->Write(leveldb::WriteOptions(), &b_prefix));
    CHECK_STATUS(db_ns_url->Write(leveldb::WriteOptions(), &b_url));

    DLOG(INFO) << "Batch update complete. (statements added: " << stats.added_stmts
            << ", statements removed: " << stats.removed_stmts
            << ", namespaces added: " << stats.added_ns
            << ", namespaces removed: " << stats.removed_ns
            << ", time=" << std::chrono::duration <double, std::milli> (
                std::chrono::steady_clock::now() - start).count() << "ms).";

    return stats;
}

void LevelDBPersistence::AddNamespace(
        const Namespace &ns, WriteBatch &ns_prefix, WriteBatch &ns_url) {
    DLOG(INFO) << "Adding namespace " << ns.DebugString();

    std::string buffer;
    ns.SerializeToString(&buffer);
    ns_prefix.Put(ns.prefix(), buffer);
    ns_url.Put(ns.uri(), buffer);
}

void LevelDBPersistence::RemoveNamespace(
        const Namespace &pattern, WriteBatch &ns_prefix, WriteBatch &ns_url) {
    DLOG(INFO) << "Removing namespaces matching pattern " << pattern.DebugString();

    GetNamespaces(pattern, [&ns_prefix, &ns_url](const rdf::proto::Namespace& ns){
        ns_prefix.Delete(ns.prefix());
        ns_url.Delete(ns.uri());
    });
}


void LevelDBPersistence::AddStatement(
        const Statement &stmt,
        WriteBatch &spoc, WriteBatch &cspo, WriteBatch &opsc, WriteBatch &pcos) {
    DLOG(INFO) << "Adding statement " << stmt.DebugString();

    std::string buffer, bufs, bufp, bufo, bufc;

    stmt.SerializeToString(&buffer);

    stmt.subject().SerializeToString(&bufs);
    stmt.predicate().SerializeToString(&bufp);
    stmt.object().SerializeToString(&bufo);
    stmt.context().SerializeToString(&bufc);

    char *k_spoc = (char *) calloc(4 * KEY_LENGTH, sizeof(char));
    computeKey(&bufs, &bufp, &bufo, &bufc, k_spoc);
    spoc.Put(leveldb::Slice(k_spoc, 4 * KEY_LENGTH), buffer);
    free(k_spoc);

    char *k_cspo = (char *) calloc(4 * KEY_LENGTH, sizeof(char));
    computeKey(&bufc, &bufs, &bufp, &bufo, k_cspo);
    cspo.Put(leveldb::Slice(k_cspo, 4 * KEY_LENGTH), buffer);
    free(k_cspo);

    char *k_opsc = (char *) calloc(4 * KEY_LENGTH, sizeof(char));
    computeKey(&bufo, &bufp, &bufs, &bufc, k_opsc);
    opsc.Put(leveldb::Slice(k_opsc, 4 * KEY_LENGTH), buffer);
    free(k_opsc);

    char *k_pcos = (char *) calloc(4 * KEY_LENGTH, sizeof(char));
    computeKey(&bufp, &bufc, &bufo, &bufs, k_pcos);
    pcos.Put(leveldb::Slice(k_pcos, 4 * KEY_LENGTH), buffer);
    free(k_pcos);
}


int64_t LevelDBPersistence::RemoveStatements(
        const Statement& pattern,
        WriteBatch& spoc, WriteBatch& cspo, WriteBatch& opsc, WriteBatch&pcos) {
    DLOG(INFO) << "Removing statements matching " << pattern.DebugString();

    int64_t count = 0;

    std::string bufs, bufp, bufo, bufc;
    GetStatements(pattern, [&](const Statement stmt) {
        stmt.subject().SerializeToString(&bufs);
        stmt.predicate().SerializeToString(&bufp);
        stmt.object().SerializeToString(&bufo);
        stmt.context().SerializeToString(&bufc);

        char* k_spoc = (char*)calloc(4 * KEY_LENGTH, sizeof(char));
        computeKey(&bufs, &bufp, &bufo, &bufc, k_spoc);
        spoc.Delete(leveldb::Slice(k_spoc, 4 * KEY_LENGTH));
        free(k_spoc);

        char* k_cspo = (char*)calloc(4 * KEY_LENGTH, sizeof(char));
        computeKey(&bufc, &bufs, &bufp, &bufo, k_cspo);
        cspo.Delete(leveldb::Slice(k_cspo, 4 * KEY_LENGTH));
        free(k_cspo);

        char* k_opsc = (char*)calloc(4 * KEY_LENGTH, sizeof(char));
        computeKey(&bufo, &bufp, &bufs, &bufc, k_opsc);
        opsc.Delete(leveldb::Slice(k_opsc, 4 * KEY_LENGTH));
        free(k_opsc);

        char* k_pcos = (char*)calloc(4 * KEY_LENGTH, sizeof(char));
        computeKey(&bufp, &bufc, &bufo, &bufs, k_pcos);
        pcos.Delete(leveldb::Slice(k_pcos, 4 * KEY_LENGTH));
        free(k_pcos);

        count++;
    });

    return count;
}

int KeyComparator::Compare(const leveldb::Slice& a, const leveldb::Slice& b) const {
    for (int i=0; i < 4 * KEY_LENGTH; i++) {
        unsigned char ac = (unsigned char)a.data()[i];
        unsigned char bc = (unsigned char)b.data()[i];
        if (ac < bc) {
            return -1;
        } else if (ac > bc) {
            return 1;
        }
    }
    return 0;
}


}  // namespace persistence
}  // namespace marmotta


