#pragma once
enum class DNSReplyCode { NoError = 0, ServerFailure = 2, NonExistentDomain = 3 };
struct DNSServer { template<class... A> bool start(A...){return true;} void stop(){} void processNextRequest(){} template<class... A> void setErrorReplyCode(A...){} };
