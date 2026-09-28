# Payment Duplicates Gateway Simulator

A small C++17 payment gateway simulator demonstrating request validation,
acquirer authorization, retry handling, idempotency, and concurrent request
handling. It uses an in-memory bank/acquirer and does not connect to real
payment services.

## Files

- `payment_simulator.hpp` — request/response types, the sample `Bank`, and the
  thread-safe `PaymentGateway`.
- `payment_simulator.cpp` — runnable example that submits an approved payment
  and a payment declined by the bank's amount limit.
- `payment_simulator_tests.cpp` — standalone tests for gateway behavior.

## Build and run

Compile and run the example from the repository root:

```sh
c++ -std=c++17 -Wall -Wextra -Wpedantic -pthread payment_simulator.cpp -o payment_simulator
./payment_simulator
```

Compile and run the test executable:

```sh
c++ -std=c++17 -Wall -Wextra -Wpedantic -pthread payment_simulator_tests.cpp -o payment_simulator_tests
./payment_simulator_tests
```

## Gateway behavior

- Rejects requests with a missing request ID or account ID, or a non-finite or
  non-positive amount.
- Uses the request ID as an idempotency key. Repeated submissions with the same
  payment details return the saved definitive result without authorizing again;
  reusing the key with different details fails.
- Coordinates concurrent duplicates so only one authorization is in flight for
  a given request ID.
- Retries pending authorizations up to the configured maximum number of
  attempts. If the outcome remains unknown, it returns `Pending` and allows the
  same request ID to be retried later.
- The sample `Bank` approves amounts up to and including 5000 and declines
  larger amounts.

This is an educational simulator, not production payment software. Its
idempotency records are held in memory and are lost when the process exits.
