## A FIX Protocol front-end for Interactive Brokers Trader Workstation or IB Gateway

### Motivation ###
Sometimes you need a FIX host to test things. Interactive Brokers offers one, but it requires setup and minimum commissions. This builds a FIX server that uses the IB API on the backend, and a FIX Protocol front-end. This is useful for FIX testing as well as keeping your automated strategies broker-generic.

## How it works

```
 strategy ──FIX 4.2──▶ ib_fix_gateway ──TWS API──▶ TWS or IB Gateway ──▶ Interactive Brokers
 strategy ──FIX 4.2──▶   (acceptor)
```

`ib_fix_gateway` is a FIX 4.2 acceptor. Each strategy connects as a FIX initiator with its own
SenderCompID. New orders, cancels and cancel/replaces are translated into TWS API calls on a
single API connection to TWS (or IB Gateway); TWS's order status, execution and error callbacks
come back to the strategy that owns the order as ExecutionReports and OrderCancelRejects.

The code is in three libraries plus a small `main()`:

| Directory | Library | Role |
|---|---|---|
| `src/fix` | `fix_acceptor` | FIX 4.2 sessions (FIX8), message decoding/encoding, order validation |
| `src/ib` | `ib_bridge` | translation to TWS orders, per-order state, the TWS connection and reconnection |
| `src/engine` | `ib_fix_engine` | `Engine` (constructed from an `EngineConfig`) wires the two together |
| `src/main.cpp` | `ib_fix_gateway` | reads the config file, runs the engine until Ctrl-C / SIGTERM |

Dependencies are git submodules: [FIX8](https://github.com/jmjatlanta/fix8) (FIX engine) and
[ib_helper](https://github.com/jmjatlanta/ib_helper) (TWS API helpers), both on their
`subproject-support` branches.

## Deployment

### 1. Requirements

* macOS or Linux, a C++20 compiler and CMake 3.25 or newer. Developed and tested on macOS with
  Apple clang 21 and CMake 4.4; Linux should work (the code is POSIX) but has not been tested.
* The **TWS API source**, version 10.37 or newer (the protobuf-based API), downloaded from
  Interactive Brokers. The build needs its `source` directory, which contains `cppclient/` and
  `proto/`; by default it is looked for in `~/IBJts/source`. The build files refer to
  `CppClient/client`, which only matches `cppclient/client` on a case-insensitive file system
  (macOS); on Linux add a link: `ln -s cppclient ~/IBJts/source/CppClient`.
* Installed libraries: Boost (`log`, `log_setup`) 1.74+, Protobuf (with `protoc`), OpenSSL,
  zlib and libcurl. On macOS: `brew install cmake boost protobuf openssl@3`.
* Network access the first time CMake configures: FIX8's build downloads POCO and oneTBB, and the
  tests download GoogleTest.
* At run time: TWS or IB Gateway, logged in, with the API enabled (see step 3).

### 2. Build

```bash
git clone --recursive <repository url> ib_fix      # or, in an existing clone:
git submodule update --init --recursive            #   fetch FIX8, ib_helper and their submodules
cd ib_fix
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DIB_CLIENT_SOURCE_DIR=$HOME/IBJts/source
cmake --build build -j
ctest --test-dir build                             # offline tests; none of them place orders
```

The gateway is `build/src/ib_fix_gateway`.

* Use `-DCMAKE_BUILD_TYPE=Release` for deployment; without a build type the project builds Debug.
* `HH_DATELIB` (Howard Hinnant's date library, used by ib_helper) is on by default on macOS,
  whose standard library lacks `std::chrono` time zones. Elsewhere it is off; turn it on
  (`-DHH_DATELIB=ON`) if the compiler's library lacks them too.
* **There is no install step yet.** The executable finds the FIX8, POCO and TBB shared libraries
  through absolute paths into the build directory, and the other libraries where they were
  installed. Build on the machine that runs the gateway, and keep the build directory where it is.

### 3. Configure TWS or IB Gateway

In TWS: *File / Global Configuration / API / Settings* (IB Gateway: *Configure / Settings / API*):

* **Enable ActiveX and Socket Clients**: on.
* **Socket port**: the defaults are TWS 7497 (paper) / 7496 (live) and IB Gateway 4002 (paper) /
  4001 (live).
* **Read-Only API**: off, or TWS refuses every order.
* **Trusted IPs**: add 127.0.0.1 (or the gateway's address), or accept the connection prompt.
* API orders far from the market can trip *API / Precautions*. If orders are rejected with a
  precaution message, review those settings or enable "Bypass Order Precautions for API Orders".

For an unattended deployment prefer IB Gateway, and configure its daily auto-restart (or
auto-logoff) to suit your trading hours: the gateway reconnects on its own when TWS / IB Gateway
comes back (step 6).

### 4. Configure the gateway

The gateway reads two files: its own settings (`config/gateway.cfg`) and the FIX8 configuration
of the FIX acceptor (`config/acceptor.xml`). Copy both to a directory for the deployment, e.g.
`/opt/ib_fix/etc/`, together with an empty `run/` directory for FIX8's logs and sequence numbers.

**`gateway.cfg`** — one `key = value` per line, `#` comments; unknown keys are errors.
Relative paths are relative to this file.

| Key | Default | Meaning |
|---|---|---|
| `host` | 127.0.0.1 | where TWS / IB Gateway runs |
| `port` | 7497 | its API socket port |
| `client_id` | 1 | API client id; must differ from every other API client of that TWS. Keep it the same between runs: TWS ties orders to it |
| `account` | (empty) | account for orders without Account (tag 1); must be one of the logged-in accounts. **Required** for logins with several accounts (e.g. an advisor's master + sub-accounts), or TWS rejects such orders (error 436) |
| `connect_timeout_seconds` | 10 | how long to wait for TWS to accept the connection |
| `reconnect` | true | reconnect automatically when the connection to TWS is lost |
| `reconnect_max_interval_seconds` | 30 | longest wait between reconnection attempts (1 s, 2 s, 4 s, ... up to this) |
| `order_journal` | (none) | file where order state is saved, so that after a restart the gateway takes back its orders still working in TWS (step 6). The sample uses `run/orders.journal`. Without it a restarted gateway does not know its earlier orders |
| `fix_config` | (required) | the FIX8 XML file |
| `fix_session` | IBFIX | the `<session>` in that file to run |
| `fix_global_log` | (FIX8 default) | FIX8's process-wide log file |

**`acceptor.xml`** — standard FIX8 configuration. The settings that matter most:

* `<session ... ip="127.0.0.1" port="9878" sender_comp_id="IBFIX">`: where strategies connect and
  the gateway's CompID (strategies use it as their TargetCompID). To accept connections from
  other machines, set `ip="0.0.0.0"` and restrict who may log on (see Security).
* `persist="file_persist"` with `<persist ... type="file" dir="./run">`: per-strategy sequence
  numbers and sent messages, so a strategy can reconnect and resend-request across gateway
  restarts. `reset_sequence_numbers="false"` keeps them; a strategy can still reset by sending
  ResetSeqNumFlag (141=Y) in its Logon.
* `session_log` / `protocol_log`: per-strategy session and message logs.
* `client_group` (commented out in the sample): the CompIDs, and optionally IP addresses, allowed
  to log on.

FIX8 takes the paths *inside* `acceptor.xml` relative to the **working directory**, not to the
file. Either start the gateway from the directory containing `run/`, or change `dir=` and the
log `filename=` attributes to absolute paths (recommended for a service).

### 5. Run

```bash
cd /opt/ib_fix/etc
/path/to/ib_fix/build/src/ib_fix_gateway gateway.cfg
```

On start-up the gateway connects to TWS, checks the configured account against TWS's accounts,
starts listening for FIX connections, and prints `ib_fix: Running. Press Ctrl-C to stop.`
Progress messages go to standard output (prefixed `ib_fix:`), as do ib_helper's TWS messages.

Ctrl-C or SIGTERM shuts it down cleanly: connected strategies are sent a Logout ("Gateway shutting
down"), the acceptor stops, and the TWS connection is closed. Exit status: `0` after a clean
shutdown, `1` if it could not start (unreadable configuration, TWS unreachable within
`connect_timeout_seconds`, unknown account, FIX port in use), `2` for a usage error.

**As a systemd service (Linux):**

```ini
# /etc/systemd/system/ib_fix.service
[Unit]
Description=ib_fix FIX 4.2 gateway to Interactive Brokers
After=network-online.target

[Service]
User=ibfix
WorkingDirectory=/opt/ib_fix/etc
ExecStart=/opt/ib_fix/build/src/ib_fix_gateway /opt/ib_fix/etc/gateway.cfg
# the gateway exits if TWS is not up yet when it starts: try again later
Restart=on-failure
RestartSec=30
KillSignal=SIGTERM
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
```

**On macOS**, a launchd agent (`~/Library/LaunchAgents/…plist`) with `ProgramArguments` set to
the gateway and its config file, `WorkingDirectory` set to the deployment directory, and
`KeepAlive` → `SuccessfulExit` = false gives the same behaviour (launchd stops jobs with SIGTERM).

### 6. Operation

* **Connection to TWS.** If the connection is lost (TWS or IB Gateway restarted, network failure),
  the gateway reconnects automatically. While it is down, new orders are rejected
  (ExecutionReport, ExecType 8, Text "Not connected to TWS") and cancels/replaces get an
  OrderCancelReject. After reconnecting it asks TWS for the day's executions and for its open
  orders: fills that happened meanwhile are reported; an order that is neither filled nor still
  open is reported Canceled ("no longer open in TWS"); a cancel or replace that TWS did not
  confirm is rejected so the strategy can resend it. Error 1100 (TWS lost its own connection to
  IB) also blocks orders until TWS reports the connection restored.
* **Strategies disconnecting.** Their orders keep working. Reports produced while a strategy is
  logged out are queued and sent, in order, when it logs on again.
* **Gateway restarts.** With `order_journal` set, each order's state is saved as its strategy is
  told about it (owner, ClOrdIDs, terms, status, filled quantity, TWS ExecIDs reported, any
  pending cancel or replace). On start-up the gateway loads the journal and catches up with TWS
  exactly as after a reconnect, so each strategy is told, when it logs on, about fills, cancels
  and unconfirmed requests from while the gateway was down; fills it already had are not
  repeated. Recovered orders are cancelled and replaced by ClOrdID as before, and their ClOrdIDs
  stay in use. Delivery is at least once: a report sent just before a crash but not yet saved is
  sent again after the restart, with the same ExecID. Keep `client_id` unchanged between runs
  (TWS only reports the orders and executions of the same API client id) and do not run two
  gateways on one journal. On loading, the journal is compacted and orders that finished before
  today are dropped; orders open in TWS under the client id but missing from the journal are
  reported in the log and left alone.
* **One session per CompID.** A second Logon with a SenderCompID that is already logged on is
  refused.
* **Logs.** Gateway messages on standard output; per-strategy FIX session and protocol logs and
  FIX8's global log where `acceptor.xml` / `fix_global_log` put them.

### 7. Connecting a strategy

FIX 4.2, TCP to the acceptor's `ip`/`port`, TargetCompID = the acceptor's `sender_comp_id`
(IBFIX in the sample), SenderCompID = any CompID not already logged on (or one allowed by the
`client_group`).

| Strategy sends | Gateway answers |
|---|---|
| NewOrderSingle (D) | ExecutionReport PendingNew (A) when sent to TWS, then New (0), PartiallyFilled (1) / Filled (2) for each execution, Canceled (4); Rejected (8) if invalid or refused by TWS |
| OrderCancelRequest (F) | ExecutionReport Canceled (4), or OrderCancelReject (9) |
| OrderCancelReplaceRequest (G) | ExecutionReport Replaced (5), or OrderCancelReject (9) |
| anything else | BusinessMessageReject (j), reason 3 (unsupported message type) |

Order fields used: ClOrdID (11), HandlInst (21, required by FIX 4.2), Symbol (55), Side (54:
1 buy, 2 sell, 5 sell short — sent to IB as SELL), OrderQty (38), OrdType (40: 1 market,
2 limit, 3 stop, 4 stop limit), Price (44), StopPx (99), TimeInForce (59: 0 day, 1 GTC, 2 at the
opening, 3 IOC, 4 FOK; default day), Account (1), SecurityType (167: CS or absent), Currency (15,
default USD), SecurityExchange (207, default SMART), TransactTime (60). ClOrdIDs must be unique
per strategy; after a replace, the order is known by its new ClOrdID. One cancel or replace may be
outstanding per order. In ExecutionReports, OrderID (37) is the TWS order id and fills carry
TWS's own ExecID (17).

### 8. Testing a deployment

`build/test/ib_fix_tws_tests` sends real orders through the gateway's components to a TWS
**paper trading** session: limit orders far from the market (placed, replaced, canceled), an
unknown symbol, and, while the market is open, a one-share market order that is then sold again;
it also cuts the TWS connection through a proxy to check reconnection, and restarts the engine to
check that its orders are recovered. It refuses to trade unless
every account is a paper account. See `test/test_resources/README.md` for its settings
(`test/test_resources/tws_paper.cfg`, default 127.0.0.1:7497).

### Security

* The FIX acceptor has no authentication beyond CompIDs and, through `client_group`, IP
  addresses. Keep it on 127.0.0.1, or on a private network with `client_group` set and a
  firewall in front. FIX8 supports TLS sessions; this has not been tried with ib_fix.
* Anyone who can log on can trade in the configured account. Use a paper account until the setup
  is proven, and check `account`, `port` and `client_id` before switching to live trading.

### Limitations

* Stocks only (SecurityType CS); futures and options need more FIX fields decoded. No GTD
  orders (TimeInForce 6).
* Restart recovery relies on TWS's executions for the current day: fills from a previous day
  that were never delivered (gateway down over midnight) are not recovered.
* If TWS is not reachable when the gateway starts, it exits (reconnection only applies once the
  first connection has succeeded); use the service manager's restart.
* No install target: deploy from the build directory (see step 2).
