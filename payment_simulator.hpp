#ifndef PAYMENT_SIMULATOR_HPP
#define PAYMENT_SIMULATOR_HPP

#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>

struct PaymentRequest {
    std::string requestId;
    std::string accountId;
    double amount;
};

enum class PaymentStatus {
    Approved,
    Declined,
    Pending,
    Failed
};

struct Response {
    std::string requestId;
    PaymentStatus status;
    std::string message;
};

class Acquirer {
public:
    virtual ~Acquirer() = default;
    virtual Response process(const PaymentRequest& request) = 0;
};

class Bank final : public Acquirer {
public:
    Response process(const PaymentRequest& request) override {
        if (request.amount > 5000.0) {
            return {request.requestId, PaymentStatus::Declined,
                    "Amount exceeds the bank limit"};
        }
        return {request.requestId, PaymentStatus::Approved, "Payment approved"};
    }
};

class PaymentGateway {
public:
    explicit PaymentGateway(Acquirer& acquirer, std::size_t maxAttempts = 2)
        : acquirer_(acquirer), maxAttempts_(maxAttempts == 0 ? 1 : maxAttempts) {}

    Response submit(const PaymentRequest& request) {
        const Response invalid = validate(request);
        if (invalid.status == PaymentStatus::Failed) {
            return invalid;
        }

        const std::string fingerprint = request.accountId + "\n" + std::to_string(request.amount);
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            auto it = records_.find(request.requestId);
            if (it == records_.end()) {
                records_.emplace(request.requestId, Record{fingerprint, true, {}});
                break;
            }
            if (it->second.fingerprint != fingerprint) {
                return {request.requestId, PaymentStatus::Failed,
                        "Idempotency key was reused with different payment details"};
            }
            if (it->second.processing) {
                condition_.wait(lock, [&] {
                    const auto current = records_.find(request.requestId);
                    return current == records_.end() || !current->second.processing;
                });
                continue;
            }
            return it->second.response;
        }
        lock.unlock();

        Response response = authorizeWithRetry(request);
        const bool definitive = response.status == PaymentStatus::Approved ||
                                response.status == PaymentStatus::Declined;

        lock.lock();
        auto it = records_.find(request.requestId);
        if (definitive) {
            it->second.response = response;
            it->second.processing = false;
        } else {
            records_.erase(it);
        }
        lock.unlock();
        condition_.notify_all();
        return response;
    }

private:
    struct Record {
        std::string fingerprint;
        bool processing;
        Response response;
    };

    static Response validate(const PaymentRequest& request) {
        if (request.requestId.empty() || request.accountId.empty() ||
            !std::isfinite(request.amount) || request.amount <= 0.0) {
            return {request.requestId, PaymentStatus::Failed,
                    "Request ID and account ID are required; amount must be finite and positive"};
        }
        return {request.requestId, PaymentStatus::Pending, {}};
    }

    Response authorizeWithRetry(const PaymentRequest& request) {
        for (std::size_t attempt = 0; attempt < maxAttempts_; ++attempt) {
            Response response;
            try {
                response = acquirer_.process(request);
            } catch (const std::exception&) {
                response = {request.requestId, PaymentStatus::Pending,
                            "Authorization outcome is unknown after a downstream error"};
            } catch (...) {
                response = {request.requestId, PaymentStatus::Pending,
                            "Authorization outcome is unknown after a downstream error"};
            }

            if (response.requestId != request.requestId ||
                (response.status != PaymentStatus::Approved &&
                 response.status != PaymentStatus::Declined &&
                 response.status != PaymentStatus::Pending &&
                 response.status != PaymentStatus::Failed)) {
                return {request.requestId, PaymentStatus::Failed,
                        "Acquirer returned a malformed response"};
            }
            if (response.status == PaymentStatus::Approved ||
                response.status == PaymentStatus::Declined ||
                response.status == PaymentStatus::Failed) {
                return response;
            }
        }
        return {request.requestId, PaymentStatus::Pending,
                "Authorization outcome is unknown; retry with the same request ID"};
    }

    Acquirer& acquirer_;
    const std::size_t maxAttempts_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<std::string, Record> records_;
};

#endif
