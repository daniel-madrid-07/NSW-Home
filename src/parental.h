// NSW-Home's own parental control: a PIN and a daily play-time limit per user.
// Nothing here touches the system; it is plain arithmetic over what the
// configuration already holds, so it is the same on every platform.
//
// The PIN is never stored. What is kept (in cfg.console["parental.pin"]) is
// "<salt hex>:<hash hex>", the hash being SHA-256 over salt + PIN, repeated.
// A four-digit PIN has only 10 000 values, so no hash can keep it from someone
// who reads the file and tries them all: the point is that the file does not
// show it and that a child at the console cannot read it off the screen.
#pragma once
#include <cstdint>
#include <string>

namespace parental {

// "<salt>:<hash>" for a new PIN (a fresh random salt each time).
std::string MakePin(const std::string& pin);
// Whether `pin` is the one `stored` was made from. False for a malformed `stored`.
bool CheckPin(const std::string& stored, const std::string& pin);

// SHA-256 of `data` as 64 hex digits (exposed for the self-test).
std::string Sha256Hex(const std::string& data);

// Seconds played today, from running totals: `total` is everything the user
// has ever played, `base` what that total was when `baseDay` began. A new day
// (or a total that went backwards, which means the history was edited) starts
// from the present total. Updates `base`/`baseDay` in place.
uint64_t PlayedToday(uint64_t total, uint64_t& base, std::string& baseDay, const std::string& today);

} // namespace parental
