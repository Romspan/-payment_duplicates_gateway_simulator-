#include "payment_simulator.hpp"

#include <iomanip>
#include <iostream>

int main() {
    Bank bank;
    PaymentGateway gateway(bank);
    const PaymentRequest requests[] = {
        {"pay-001", "acct-123", 49.99},
        {"pay-002", "acct-456", 7500.00},
    };

    for (const auto& request : requests) {
        std::cout << "\nPaymentRequest: " << request.requestId
                  << " ($" << std::fixed << std::setprecision(2) << request.amount << ")\n";
        const Response response = gateway.submit(request);
        const char* status = response.status == PaymentStatus::Approved ? "APPROVED" :
                             response.status == PaymentStatus::Declined ? "DECLINED" :
                             response.status == PaymentStatus::Pending ? "PENDING" : "FAILED";
        std::cout << "Response: " << status << " - " << response.message << '\n';
    }
}
