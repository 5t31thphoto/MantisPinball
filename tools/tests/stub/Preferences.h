#pragma once
struct String { const char* c_str() const; };
class Preferences { public: bool begin(const char*, bool); void end(); size_t putString(const char*, const char*); String getString(const char*, const char*); };
