#include "payment_simulator.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScriptedAcquirer final : public Acquirer {
public:
    explicit ScriptedAcquirer(std::vector<Response> responses = {})
        : responses_(std::move(responses)) {}

    Response process(const PaymentRequest& request) override {
        ++calls;
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        if (throws) {
            throw std::runtime_error("simulated downstream exception");
        }
        if (next_ < responses_.size()) {
            return responses_[next_++];
        }
        return {request.requestId, PaymentStatus::Approved, "Payment approved"};
    }

    std::atomic<int> calls{0};
    std::chrono::milliseconds delay{0};
    bool throws{false};

private:
    std::vector<Response> responses_;
    std::size_t next_{0};
};

void happyPath() {
    Bank bank;
    PaymentGateway gateway(bank);
    const Response response = gateway.submit({"happy", "acct-1", 42.50});
    expect(response.status == PaymentStatus::Approved, "happy path should approve");
}

void invalidRequests() {
    ScriptedAcquirer acquirer;
    PaymentGateway gateway(acquirer);
    expect(gateway.submit({"", "acct-1", 10}).status == PaymentStatus::Failed,
           "empty request ID should fail");
    expect(gateway.submit({"invalid-amount", "acct-1", 0}).status == PaymentStatus::Failed,
           "zero amount should fail");
    expect(gateway.submit({"nan-amount", "acct-1", std::nan("")}).status == PaymentStatus::Failed,
           "non-finite amount should fail");
    expect(acquirer.calls == 0, "invalid requests must not reach the acquirer");
}

void timeoutThenRetry() {
    ScriptedAcquirer acquirer({
        {"timeout", PaymentStatus::Pending, "simulated timeout"},
        {"timeout", PaymentStatus::Approved, "Payment approved"},
    });
    PaymentGateway gateway(acquirer, 2);
    const Response response = gateway.submit({"timeout", "acct-1", 12});
    expect(response.status == PaymentStatus::Approved, "retry should recover a timed-out request");
    expect(acquirer.calls == 2, "timeout should be retried once");
}

void duplicateTransactionAndIdempotency() {
    ScriptedAcquirer acquirer;
    PaymentGateway gateway(acquirer);
    const PaymentRequest request{"duplicate", "acct-1", 25};
    const Response first = gateway.submit(request);
    const Response second = gateway.submit(request);
    expect(first.status == PaymentStatus::Approved && second.status == PaymentStatus::Approved,
           "duplicate submission should return the original result");
    expect(acquirer.calls == 1, "duplicate transaction must reach acquirer only once");

    const Response conflict = gateway.submit({"duplicate", "acct-1", 30});
    expect(conflict.status == PaymentStatus::Failed,
           "reusing idempotency key with different details should fail");
    expect(acquirer.calls == 1, "conflicting duplicate must not reach acquirer");
}

void concurrentTransactions() {
    ScriptedAcquirer acquirer;
    acquirer.delay = std::chrono::milliseconds(40);
    PaymentGateway gateway(acquirer);
    std::vector<Response> results(8);
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < results.size(); ++i) {
        workers.emplace_back([&, i] {
            results[i] = gateway.submit({"concurrent-" + std::to_string(i), "acct-1", 7});
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (const auto& response : results) {
        expect(response.status == PaymentStatus::Approved,
               "concurrent transaction should receive approved result");
    }
    expect(acquirer.calls == 8, "distinct concurrent transactions should all reach acquirer");

    ScriptedAcquirer duplicateAcquirer;
    duplicateAcquirer.delay = std::chrono::milliseconds(40);
    PaymentGateway duplicateGateway(duplicateAcquirer);
    workers.clear();
    for (std::size_t i = 0; i < results.size(); ++i) {
        workers.emplace_back([&, i] {
            results[i] = duplicateGateway.submit({"concurrent-duplicate", "acct-1", 7});
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (const auto& response : results) {
        expect(response.status == PaymentStatus::Approved,
               "concurrent duplicate should receive approved result");
    }
    expect(duplicateAcquirer.calls == 1,
           "concurrent duplicates must produce one acquirer call");
}

void bankDecline() {
    Bank bank;
    PaymentGateway gateway(bank);
    const Response response = gateway.submit({"decline", "acct-1", 5001});
    expect(response.status == PaymentStatus::Declined, "bank limit should decline payment");
}

void gatewayFailureAndRecovery() {
    ScriptedAcquirer acquirer;
    acquirer.throws = true;
    PaymentGateway gateway(acquirer, 1);
    const PaymentRequest request{"gateway-failure", "acct-1", 10};
    expect(gateway.submit(request).status == PaymentStatus::Pending,
           "downstream exception should produce an unknown/pending result");
    acquirer.throws = false;
    expect(gateway.submit(request).status == PaymentStatus::Approved,
           "same request should recover after transient failure");
    expect(acquirer.calls == 2, "recovery should retry through the acquirer");
}

void networkFailureAndRecovery() {
    ScriptedAcquirer acquirer({
        {"network", PaymentStatus::Pending, "network unavailable"},
        {"network", PaymentStatus::Approved, "Payment approved"},
    });
    PaymentGateway gateway(acquirer, 1);
    const PaymentRequest request{"network", "acct-1", 10};
    expect(gateway.submit(request).status == PaymentStatus::Pending,
           "network failure should return pending when attempts are exhausted");
    expect(gateway.submit(request).status == PaymentStatus::Approved,
           "same idempotency key should recover after network restoration");
}

void malformedResponse() {
    ScriptedAcquirer acquirer({{"some-other-request", PaymentStatus::Approved, "wrong correlation"}});
    PaymentGateway gateway(acquirer, 1);
    const Response response = gateway.submit({"malformed", "acct-1", 10});
    expect(response.status == PaymentStatus::Failed, "mis-correlated response should fail");
    expect(response.message == "Acquirer returned a malformed response",
           "malformed response should have a clear failure reason");
}

void run(const char* name, void (*test)(), int& failures) {
    try {
        test();
        std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
}

} // namespace

int main() {
    int failures = 0;
    run("happy path", happyPath, failures);
    run("invalid requests", invalidRequests, failures);
    run("timeout and retry", timeoutThenRetry, failures);
    run("duplicate transaction and idempotency", duplicateTransactionAndIdempotency, failures);
    run("concurrent transactions", concurrentTransactions, failures);
    run("bank decline", bankDecline, failures);
    run("gateway failure and recovery", gatewayFailureAndRecovery, failures);
    run("network failure and recovery", networkFailureAndRecovery, failures);
    run("malformed response", malformedResponse, failures);
    std::cout << (9 - failures) << "/9 tests passed\n";
    return failures == 0 ? 0 : 1;
}
