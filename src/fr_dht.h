// A small client of the BitTorrent Mainline DHT (BEP 5), used only as a
// store of signed values: BEP 44 mutable items, read with `get` and written
// with `put`. It announces itself as read-only (BEP 43), so no node keeps it
// in its routing table or sends it queries: NSW-Home takes part as a client
// and nothing in the network comes to depend on it.
//
// One Client belongs to one thread; every call blocks until it is done, the
// time allowed has passed, or the stop flag is raised.
#pragma once
#include "fr_net.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dht {

// The largest value a node stores (BEP 44), counted on its bencoded form.
constexpr size_t kMaxValue = 1000;

struct Item {
    std::string value; // the stored byte string (NSW-Home only stores strings)
    int64_t seq = 0;
};

struct GetResult {
    bool reached = false;    // some node of the network answered
    std::vector<Item> items; // every different correctly signed value found, newest first
    int64_t maxSeq = -1;     // the highest sequence number any node reported
    int queried = 0, replied = 0, holders = 0;
    double seconds = 0;
};

struct PutResult {
    bool reached = false;
    int sent = 0, stored = 0;
    int conflicts = 0; // nodes that hold a newer value than the one this write replaces
    int queried = 0;   // queries of the lookup that came before the write, if one was needed
    std::string errors; // the error codes nodes answered with, for the tests' report
    double seconds = 0;
};

// Where the item of a public key and salt lives: SHA-1(key + salt).
std::string Target(const std::string& pub, const std::string& salt);
// The bytes a mutable item's signature is over.
std::string SignBuffer(const std::string& salt, int64_t seq, const std::string& value);

class Client {
public:
    explicit Client(std::atomic<bool>* stop);

    // Queries a second, at most (answers cost nothing: they are only read).
    void SetRate(double perSecond) { rate_ = perSecond; }

    // Looks the item up. With `knownSeq` >= 0 nodes are told which version
    // is already known and leave the value out when they have nothing newer.
    GetResult Get(const std::string& pub, const std::string& salt, int64_t knownSeq = -1, double maxSeconds = 20);
    // Stores the item on the nodes closest to it. Uses the nodes and write
    // tokens of the last lookup of this item when they are fresh, otherwise
    // looks it up first. `refresh`: the value is written again under the
    // sequence number it already has (nodes keep it longer; nothing changes).
    PutResult Put(const std::string& pub, const std::string& salt, const std::string& value, int64_t seq,
                  const std::string& sig, double maxSeconds = 20, bool refresh = false);

    void Close();

    // Totals since the client was made, for the tests' traffic report.
    uint64_t packetsOut = 0, packetsIn = 0, bytesOut = 0, bytesIn = 0;

private:
    struct Cand {
        std::string id; // empty: a bootstrap router, whose id is not known
        frn::Addr addr;
        enum State { Fresh, Sent, Replied, Failed } st = Fresh;
        bool findOnly = false; // asked with find_node: it knows the way but stores nothing
        bool trusted = false;  // it has answered before: one lost datagram is not the end of it
        bool retried = false;
        double sentAt = 0;
        std::string token;
        bool hasSeq = false;
        int64_t seq = 0;
        std::string dist;
    };
    struct Near {
        std::string id;
        frn::Addr addr;
        std::string token;
        bool hasSeq = false;
        int64_t seq = 0;
    };
    struct NearSet {
        std::vector<Near> nodes;
        double at = 0;
    };
    struct Known {
        std::string id;
        double at = 0;
        int strikes = 0; // lookups in a row in which it did not answer
    };

    bool Ready();
    bool Bootstrap();
    bool SendQuery(const frn::Addr& to, const std::string& packet);
    bool TakeSlot();
    void Lookup(const std::string& target, const std::string& pub, const std::string& salt, int64_t knownSeq,
                double maxSeconds, GetResult& res);
    void Remember(const frn::Addr& a, const std::string& id);

    std::atomic<bool>* stop_;
    frn::Udp udp_;
    std::string id_;
    double rate_ = 8, bucket_ = 8, bucketAt_ = 0;
    uint16_t nextTid_ = 1;
    std::vector<frn::Addr> routers_;
    double routersAt_ = -1e9;
    std::map<frn::Addr, Known> table_;       // nodes that answered lately
    std::map<frn::Addr, double> dead_;       // nodes that did not, and when
    std::map<std::string, NearSet> near_;    // per item: the nodes closest to it, with their tokens
};

} // namespace dht
