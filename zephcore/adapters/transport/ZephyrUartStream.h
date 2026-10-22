/*
 * SPDX-License-Identifier: MIT
 * Arduino Stream over an interrupt-driven Zephyr UART (CDC-ACM or plain UART),
 * for code ported from upstream that talks to `Serial` (the KISS modem).
 *
 * RX and TX each go through a ring drained by the UART ISR. A full RX ring
 * stops reading the port until the main thread has made room, so the host is
 * held off instead of losing bytes. Writes take what fits (availableForWrite()).
 * The callbacks run in the ISR: post events only.
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/ring_buffer.h>
#include <Stream.h>

class ZephyrUartStream : public Stream {
public:
	/* on_rx: bytes arrived. on_tx_idle: the TX ring has drained. */
	bool begin(const struct device *dev, void (*on_rx)(void), void (*on_tx_idle)(void));

	using Print::write;
	size_t write(uint8_t c) override { return write(&c, 1); }
	size_t write(const uint8_t *buffer, size_t size) override;
	int availableForWrite() override;

	int available() override;
	int read() override;
	int peek() override;
	/* Waits for the TX ring to drain, at most FLUSH_TIMEOUT_MS. */
	void flush() override;

private:
	static constexpr size_t RX_BUF_SIZE = 512;
	static constexpr size_t TX_BUF_SIZE = 1024;
	static constexpr uint32_t FLUSH_TIMEOUT_MS = 200;

	static void isr(const struct device *dev, void *user_data);

	const struct device *_dev = nullptr;
	void (*_on_rx)(void) = nullptr;
	void (*_on_tx_idle)(void) = nullptr;
	uint8_t _rx_data[RX_BUF_SIZE];
	uint8_t _tx_data[TX_BUF_SIZE];
	struct ring_buf _rx_ring;
	struct ring_buf _tx_ring;
	struct k_spinlock _tx_lock;
	struct k_sem _tx_drained;
	atomic_t _rx_paused = ATOMIC_INIT(0);
};
