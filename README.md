# c0pin

[![CI](https://github.com/psaux-it/c0pin/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/psaux-it/c0pin/actions/workflows/c-cpp.yml)

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

Aggressive mode applies the performance policy and additionally configures
more aggressive CPU performance settings where supported.

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

## Usage

c0pin provides **two independent operating modes**. Choose the mode that matches
your requirements. You do **not** need to run both modes.

### Performance Policy

Use this mode for a performance-oriented CPU policy without aggressive tuning
or CPU DMA latency QoS:

```bash
sudo c0pin --performance-policy
```

c0pin applies the performance policy and exits. The configured CPU policy
remains in effect until it is changed by another component or explicitly
reconfigured.

### Aggressive Mode

Use this mode when you also want the Performance Policy, aggressive CPU
performance controls, and a CPU DMA latency QoS constraint:

```bash
sudo c0pin --aggressive
```

Aggressive mode runs in the foreground while the DMA latency constraint is held.

When aggressive mode terminates, the DMA latency file descriptor is closed and
the associated QoS constraint is released.

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
The configured CPU policy remains in effect until it is changed by another
component or explicitly reconfigured.

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

The aggressive service remains active while c0pin holds the CPU DMA latency QoS
constraint.

The two provided services conflict with each other and are not intended to run
simultaneously.

## Uninstallation

If installed with the Makefile:

```bash
sudo make uninstall
```

If a service was enabled previously, disable it before uninstalling:

```bash
sudo systemctl disable c0pin-performance.service
sudo systemctl disable c0pin-aggressive.service
```

Then reload systemd:

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

## Important

c0pin does **not** save and restore the previous CPU configuration.

CPU settings changed by c0pin are not automatically reverted when the program
or service exits.

For example, stopping aggressive mode releases the `/dev/cpu_dma_latency`
constraint, but does not automatically restore previous governor, EPP, boost,
turbo, or frequency-limit settings.

Treat c0pin as a CPU policy configuration utility rather than a temporary
configuration switch.

## License

c0pin is released under the [MIT License](LICENSE).

## Author

**Hasan CALISIR**

<hasan.calisir@psauxit.com>

## Links

- [Repository](https://github.com/psaux-it/c0pin)
- [Issues](https://github.com/psaux-it/c0pin/issues)

---

c0pin is a Linux system utility for enforcing CPU performance-oriented policies.

It configures the CPU to favor performance over power efficiency through Linux
kernel interfaces. Actual CPU frequency remains subject to hardware, firmware,
thermal, power, and kernel limits.
