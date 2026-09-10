# c0pin

**c0pin** is a Linux CPU performance and latency tuner.

It provides explicit CPU performance policies for systems where predictable CPU
responsiveness is more important than power efficiency.

c0pin configures Linux kernel interfaces such as **cpufreq**, **Intel pstate**,
**amd-pstate**, CPU boost controls, and **CPU DMA latency QoS**.

## Modes

### Performance Policy

```bash
sudo c0pin --performance-policy
```

This mode:

- Sets the cpufreq governor to `performance`
- Sets EPP to `performance` where supported
- Verifies the requested configuration
- Exits after applying the policy

This mode does **not** modify:

- CPU turbo state
- CPU boost state
- CPU idle or C-state behavior
- CPU DMA latency QoS

### Aggressive

```bash
sudo c0pin --aggressive
```

Aggressive mode applies the performance policy and additionally configures more aggressive CPU performance settings where supported.

On Intel systems using Intel pstate, c0pin attempts to:

- Enable turbo operation
- Set maximum performance percentage to `100`

It also attempts to:

- Enable the kernel CPU boost mechanism
- Set the cpufreq maximum frequency to `cpuinfo_max_freq`
- Set the minimum frequency to the maximum frequency for drivers other than `amd-pstate`

Aggressive mode also acquires a CPU DMA latency QoS constraint through:

```text
/dev/cpu_dma_latency
```

The default latency bound is:

```text
0 microseconds
```

A custom latency value can be supplied:

```bash
sudo c0pin --aggressive 100
```

The DMA latency constraint remains active while c0pin is running.

## Important Behavior

c0pin does **not** save and restore the previous CPU configuration.

CPU settings changed by c0pin are not automatically reverted when the program exits.

For example, stopping aggressive mode releases the `/dev/cpu_dma_latency` constraint, but does not automatically restore previous governor, EPP, boost, turbo, or frequency-limit settings.

Treat c0pin as a CPU policy configuration utility rather than a temporary configuration switch.

## Installation

Clone the repository:

```bash
git clone https://github.com/psaux-it/c0pin.git
cd c0pin
```

Build:

```bash
make
```

Install:

```bash
sudo make install
```

The default installation paths are:

```text
/usr/local/sbin/c0pin
/usr/local/share/man/man8/c0pin.8
/usr/local/share/doc/c0pin/LICENSE
/etc/systemd/system/c0pin-performance.service
/etc/systemd/system/c0pin-aggressive.service
```

## Manual

After installation:

```bash
man 8 c0pin
```

You can also read the manual page directly from the source tree:

```bash
man ./c0pin.8
```

## Usage

### Apply Performance Policy

```bash
sudo c0pin --performance-policy
```

### Start Aggressive Mode

```bash
sudo c0pin --aggressive
```

### Start Aggressive Mode with a Custom DMA Latency Bound

```bash
sudo c0pin --aggressive 100
```

Press `Ctrl+C` to terminate aggressive mode.

`SIGTERM` is also handled for clean termination.

When aggressive mode terminates, the DMA latency file descriptor is closed and the associated QoS constraint is released.

## systemd

Two systemd service units are provided.

### Performance Policy Service

Start:

```bash
sudo systemctl start c0pin-performance.service
```

Enable at boot:

```bash
sudo systemctl enable c0pin-performance.service
```

Check status:

```bash
systemctl status c0pin-performance.service
```

This is a `oneshot` service. c0pin applies the requested policy and exits.

### Aggressive Service

Start:

```bash
sudo systemctl start c0pin-aggressive.service
```

Enable at boot:

```bash
sudo systemctl enable c0pin-aggressive.service
```

Check status:

```bash
systemctl status c0pin-aggressive.service
```

Stop:

```bash
sudo systemctl stop c0pin-aggressive.service
```

The aggressive service remains active while c0pin holds the CPU DMA latency QoS constraint.

The two provided services conflict with each other and are not intended to run simultaneously.

## Uninstallation

If installed with the Makefile:

```bash
sudo make uninstall
```

Then reload systemd if necessary:

```bash
sudo systemctl daemon-reload
```

## Build Options

The Makefile supports standard installation overrides.

For example:

```bash
make PREFIX=/usr
```

Or:

```bash
make DESTDIR=/tmp/c0pin-package PREFIX=/usr install
```

The systemd unit directory can also be overridden:

```bash
make UNITDIR=/usr/lib/systemd/system install
```

A genuine error occurred while inspecting, configuring, verifying, or acquiring a required resource.

## License

c0pin is released under the [MIT License](LICENSE).

## Author

**Hasan CALISIR**

<hasan.calisir@psauxit.com>

## Links

- [Repository](https://github.com/psaux-it/c0pin)
- [Issues](https://github.com/psaux-it/c0pin/issues)

---

c0pin is a Linux system utility for CPU performance policy configuration.

Its behavior is determined by the Linux kernel, CPU, firmware, and active cpufreq driver.
