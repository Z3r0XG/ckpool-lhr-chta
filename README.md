This repository has been archived - this version of the software is no longer being developed by Z3r0XG or HeliosPool.

# ckpool-lhr-chta

A fork of CKPool-LHR adding Cheetahcoin (CHTA) SHA256d solo mining support,
with sub-"1" difficulty for low hash rate miners and additional enhancements.

Ultra low overhead, scalable, multi-process, multi-threaded Cheetahcoin mining
pool software in C for Linux.

## Key Features

- Sub-"1" difficulty support for low hash rate miners (ESP32 and similar devices)
- Cheetahcoind cookie authentication
- User agent whitelisting
- User agent reporting (pool.status and user pages)
- Network difficulty monitoring (pool.status)
- Worker connection timestamp tracking
- Configurable donation address
- Client configurable suggested difficulty via password field

## Acknowledgment

This software is a fork of ckpool-lhr, which itself is a fork of the original CKPool by Con Kolivas.
We acknowledge Con Kolivas's foundational work and the ckpool-lhr project for making this Cheetahcoin adaptation possible.

**Parent project:** https://github.com/Z3r0XG/ckpool-lhr  
**Original project:** https://bitbucket.org/ckolivas/ckpool

## Design

**Architecture:**

- Multiprocess + multithreaded design to scale to massive deployments and capitalize on modern multicore/multithread CPU designs
- Minimal memory overhead and low-level hand-coded architecture for maximum efficiency
- Ultra-reliable Unix sockets for inter-process communication
- Modular code design to streamline further development
- Event-driven communication preventing slow clients from delaying low-latency ones

**Features:**

- Cheetahcoind communication with multiple failover to local or remote locations
- Virtually seamless restarts for upgrades through socket handover
- Configurable custom coinbase signature
- Configurable instant starting and minimum difficulty (including sub-1.0 for low hashrate miners)
- Rapid vardiff adjustment with stable unlimited maximum difficulty handling
- Password-based difficulty suggestion for clients without mining.suggest_difficulty support
- New work generation on block changes incorporate full cheetahcoind transaction set without delay
- Stratum messaging system to running clients
- Accurate pool and per-client statistics with user agent tracking
- Multiple named instances can run concurrently on the same machine

## License

GNU Public license V3. See included COPYING for details.

---

# Building

Building ckpool-lhr-chta requires basic build tools and yasm on any Linux installation. ZMQ notification support (recommended) requires the zmq devel library installed.

### Building with ZMQ (recommended)

```bash
sudo apt-get install build-essential yasm libzmq3-dev
./configure
make
```

### Basic build (without ZMQ)

```bash
sudo apt-get install build-essential yasm
./configure
make
```

### Building from git

Requires additional autotools:

```bash
git clone https://github.com/Z3r0XG/ckpool-lhr-chta.git
cd ckpool-lhr-chta
sudo apt-get install build-essential yasm autoconf automake libtool libzmq3-dev pkgconf libcmocka-dev
./autogen.sh
./configure
make
```

### Binaries

Binaries will be built in the `src/` subdirectory:

- **ckpool** - The main pool backend
- **ckpmsg** - Application for passing messages to ckpool
- **notifier** - Application for cheetahcoind's `-blocknotify` to notify ckpool of block changes

### Installation

Installation is **not required** and ckpool can be run directly from the build directory, but it can be installed system-wide with:

```bash
sudo make install
```

---

# Solo Mode

## Use Case

Solo mode allows miners to mine directly to their own Cheetahcoin address without
pooling with others. Each miner specifies their CHTA address as their
username, and any blocks found are paid directly to that address.

**Ideal for:**
- Local solo mining operations
- ESP32 and low-hashrate device mining
- Miners who want direct block rewards
- Testing and development

## How Solo Mode Works

In solo mode, ckpool-lhr-chta connects to a Cheetahcoin daemon (cheetahcoind) to receive
block templates. Miners connect and provide their CHTA address as their
username. When a block is found, the reward goes directly to the miner's
specified address.

**Workflow**: Cheetahcoind provides block template → Pool generates work → Miner
connects with CHTA address as username → Miner submits shares → Block found →
Reward sent directly to miner's address.

---

## Command Line Options

**`-B | --btcsolo`** **REQUIRED**
- Start ckpool in solo mode
- Example: `src/ckpool -B`

**`-c CONFIG | --config CONFIG`**
- Specify path to configuration file (default: ckpool-lhr-chta.conf)

**`-D | --daemonise`**
- Run ckpool as a daemon (background process)

**`-g GROUP | --group GROUP`**
- Run as specified group

**`-H | --handover`**
- Handover mode: Take over sockets from old instance, then shut it down
- Implies -k (killold)

**`-h | --help`**
- Display help message with all options

**`-k | --killold`**
- Send shutdown message to old instance with same name

**`-L | --log-shares`**
- Log shares to disk (can generate large logs)

**`-l LOGLEVEL | --loglevel LOGLEVEL`**
- Set log level (0=emergency to 7=debug, default: 5=notice)

**`-n NAME | --name NAME`**
- Set process name (default: ckpool-lhr-chta)

**`-q | --quiet`**
- Disable warnings and non-critical messages

**`-s SOCKDIR | --sockdir SOCKDIR`**
- Directory for unix domain sockets

---

## Quick Start

> This software requires a Cheetahcoin node running. Tested with [Cheetahcoin](https://github.com/ShorelineCrypto/cheetahcoin) v2.4.0.

#### 1. Configure Cheetahcoin Daemon

Edit `cheetahcoin.conf` to enable RPC:
```
server=1
rpcuser=username
rpcpassword=password
rpcallowip=127.0.0.1
rpcbind=127.0.0.1
```

Enable ZMQ block notifications (recommended):
```
zmqpubhashblock=tcp://127.0.0.1:28333
```

Or use the notifier script as an alternative:
```
blocknotify=/path/to/ckpool/src/notifier
```

#### 2. Create Configuration File

Copy the example and customize:
```bash
cp ckpool.conf.example ckpool.conf
```

Edit `ckpool.conf` with required settings:
```json
{
"btcd" : [
    {
        "url" : "127.0.0.1:8536",
        "auth" : "username",
        "pass" : "password",
        "notify" : true
    }
],
"logdir" : "logs"
}
```

#### 3. Start ckpool-lhr-chta in Solo Mode

```bash
src/ckpool -B
```

#### 4. Configure Your Miners

Point mining hardware to the pool:
- **URL**: `192.168.1.100:3333` (pool IP address, default port 3333)
- **Username**: Your CHTA address, optionally with a worker name (e.g., `CYourCHTAaddress.worker1`)
- **Password**: `x, diff=200`
    - `x` - Default password (any value accepted)
    - `diff=X` - Suggest difficulty where `X` is numeric (e.g., `diff=200` or `diff=0.001`).

Any valid CHTA address works as the username. CHTA mainnet addresses start with `C`. Append `.workername` to track multiple miners separately.

---

## Configuration Options

All configuration options are listed below.

**"btcd"** : Cheetahcoind connection configuration. **REQUIRED**
- Type: Array of objects
- Purpose: Each object defines a cheetahcoind instance connection
- Object fields:
  - **"url"** (string, required): Cheetahcoind RPC endpoint (IP:port or hostname:port)
  - **"auth"** (string, required): RPC username for authentication
  - **"pass"** (string, required): RPC password for authentication
  - **"cookie"** (string, optional): Path to cheetahcoind cookie file (alternative to auth/pass)
  - **"notify"** (boolean, optional): Enable block template notifications from this node
- Default: localhost:8536 with user "user" and password "pass" if not specified
- Note: Cookie-based authentication can be used as an alternative to auth/pass by
  specifying the cookie file path. Multiple cheetahcoind instances can be configured for redundancy.
- Example:
```json
"btcd" : [
    {
        "url" : "127.0.0.1:8536",
        "auth" : "username",
        "pass" : "password",
        "notify" : true
    }
]
```

**"donaddress"** : CHTA address for donation payments. **OPTIONAL**
- Type: String
- Values: Any valid CHTA mainnet address (starts with 'C')
- Default: not configured — set this if donation > 0
- Note: Only used when "donation" is configured and greater than 0.
- Example: `"donaddress" : "CYourCHTAaddressHere"`

**"donation"** : Percentage of block reward to donate. **OPTIONAL**
- Type: Double
- Values: 0.0 to 99.9
- Default: 0 (disabled)
- Note: Values below 0.1 are treated as 0 to avoid dust-sized donations.
- Example: `"donation" : 0.5`

**"btcsig"** : Signature to include in coinbase of mined blocks. **OPTIONAL**
- Type: String
- Values: Up to 38 bytes
- Default: None
- Example: `"btcsig" : "/solo mined/"`
- Note: Truncated to 38 bytes at load time (bytes, not characters); multibyte UTF-8 uses multiple bytes. If truncated, a warning is logged.
- To check: `echo -n "<sig>" | wc -c` (bytes)

**"serverurl"** : Server bindings for miner connections. **OPTIONAL**
- Type: Array of strings
- Values: "IP:port" or "hostname:port"
- Default: All interfaces on port 3333
- Note: Ports below 1024 usually require privileged access
- Example: `"serverurl" : ["192.168.1.100:3333", "127.0.0.1:3333"]`

**"mindiff"** : Minimum difficulty for vardiff. **OPTIONAL**
- Type: Double
- Values: Any positive number
- Default: 1
- Note: Supports sub-1 values (e.g., 0.0005) for low hash rate miners (ESP32).
- Example: `"mindiff" : 1` or `"mindiff" : 0.0005`

**"startdiff"** : Starting difficulty for new clients. **OPTIONAL**
- Type: Double
- Values: Any positive number
- Default: 42
- Note: Supports sub-1 values (e.g., 0.0005) for low hash rate miners.
- Note: Cannot be lower than `mindiff`.
- Example: `"startdiff" : 42` or `"startdiff" : 0.0005`

**"maxdiff"** : Maximum difficulty for vardiff. **OPTIONAL**
- Type: Double
- Values: Any positive number, or 0 for no maximum
- Default: 0 (no maximum)
- Note: Must be greater than `mindiff`.
- Example: `"maxdiff" : 10000000` or `"maxdiff" : 0`

**"highdiff"** : Starting difficulty for designated high-diff server ports. **OPTIONAL**
- Type: Double
- Values: Any positive number
- Default: 1000000
- Note: Automatically applied to any port > 4000 in `serverurl`.
- Note: Clamped at `mindiff` and `maxdiff`.
- Example: `"highdiff" : 10000`

**"allow_low_diff"** : Remove minimum network difficulty floor (for regtest). **OPTIONAL**
- Type: Boolean
- Default: false
- Note: Enables block submission to blockchains with sub-1.0 network difficulty (e.g., regtest).
- Warning: For regtest testing only. Do NOT enable on mainnet or testnet.
- Example: `"allow_low_diff" : true`

**"nonce1length"** : Length of extranonce1 in bytes. **OPTIONAL**
- Type: Integer
- Values: 2-8
- Default: 4

**"nonce2length"** : Length of extranonce2 in bytes. **OPTIONAL**
- Type: Integer
- Values: 2-8
- Default: 8

**"update_interval"** : Frequency to send stratum updates. **OPTIONAL**
- Type: Integer
- Values: Seconds (positive integer)
- Default: 30

**"version_mask"** : Allowed version bits for clients to modify. **OPTIONAL**
- Type: String (hex)
- Default: "1fffe000"
- Example: `"version_mask" : "1fffe000"`

**"blockpoll"** : Frequency to poll cheetahcoind for new blocks. **OPTIONAL**
- Type: Integer
- Values: Milliseconds
- Default: 5000
- Note: Only used if ZMQ is not configured. CHTA has ~2 minute blocks; 5000ms polls detect a new block within ~4% of block time.

**"zmqblock"** : ZMQ interface for block notifications. **OPTIONAL**
- Type: String
- Values: ZMQ URL format
- Default: "tcp://127.0.0.1:28333"
- Note: Requires cheetahcoind -zmqpubhashblock option. Match this to the zmqpubhashblock address in cheetahcoin.conf.

**"logdir"** : Directory for logs. **OPTIONAL**
- Type: String
- Default: "logs"

**"maxclients"** : Maximum concurrent client connections. **OPTIONAL**
- Type: Integer
- Default: 0 (auto-calculated as 90% of system's `ulimit -n`)

**"dropidle"** : Drop idle clients after this many seconds. **OPTIONAL**
- Type: Integer
- Default: 0 (disabled)
- Note: 1 hour (3600) is generous for low hash rate miners.

**"useragent"** : Allowed user agent strings (whitelist). **OPTIONAL**
- Type: Array of strings
- Default: None (all allowed)
- Note: Prefix matching. Empty user agents rejected if whitelist configured.
- Example: `"useragent" : ["cpuminer", "cgminer"]`

**"max_pool_useragents"** : Maximum distinct normalized useragents to include in pool.status. **OPTIONAL**
- Type: Integer
- Default: 100
- Note: Controls how many aggregated UA entries (normalized string + count) are included in pool/pool.status JSON; 0 disables publishing. Individual user/worker pages show the specific useragent string for each worker.

---

## Notes

> [!NOTE]
> **Difficulty rounding:** `mindiff`, `startdiff`, `maxdiff`, and `highdiff` values >= 1 are rounded to whole numbers. Values below 1 retain full precision.

> [!WARNING]
> JSON is strict with formatting. Do not put a comma after the last field.

> [!NOTE]
> You can mine with a pruned blockchain, though it may add latency.

---

## Other Modes

ckpool-lhr-chta is optimized and documented for solo mining. Although it inherits all capabilities from upstream CKPool, other modes are untested and therefore unsupported.

For documentation on pool, proxy, and passthrough modes, please refer to the [original CKPool documentation](https://bitbucket.org/ckolivas/ckpool).
