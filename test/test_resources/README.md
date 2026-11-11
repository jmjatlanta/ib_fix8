# Test resources

`tws_paper.cfg` tells the live integration tests (`ib_fix_tws_tests`, built from
`test/TwsIntegrationTest.cpp`) where TWS is. By default it points at a TWS **paper trading**
session on this machine, API port 7497. To use another file, set `IB_FIX_TWS_CONFIG` to its path.

The tests send orders through the whole gateway: a FIX 4.2 test client, the acceptor, the IB
order handler and TWS. They place, replace and cancel 1–2 share limit orders far below the
market, check that an unknown symbol is rejected, and, while the market is open, buy one share
at market and sell it again.

The reconnection tests (`test/TwsReconnectTest.cpp`) put a small TCP proxy between the gateway
and TWS and use it to close or reset the connection and to refuse new ones for a few seconds.
They check that requests are refused while TWS is unreachable, that the gateway reconnects, that
a resting order survives, and (while the market is open) that a fill whose reports were lost with
the connection is reported after reconnecting. They use API client ids `client_id + 1` and
`client_id + 2` as well as `client_id`.

## Safety

* The tests only place orders if every account TWS reports is a paper account (ids starting
  with `D`). Otherwise they fail without sending anything.
* They are not part of the default `ctest` run. Run them explicitly:
  `./build/test/ib_fix_tws_tests`, or configure with `-DIB_FIX_TWS_TESTS=ON` to add them to `ctest`.
* If TWS is not reachable, the tests are skipped.

## TWS settings (File / Global Configuration / API / Settings)

* "Enable ActiveX and Socket Clients": on
* "Socket port": 7497
* "Read-Only API": off (otherwise TWS refuses to place orders)
* Trusted IPs: 127.0.0.1, or accept the connection prompt the first time

API orders far from the market can trip TWS's order precautions (API / Precautions). If the
limit-order tests are rejected with a precaution message, enable "Bypass Order Precautions for
API Orders" for the paper account.
