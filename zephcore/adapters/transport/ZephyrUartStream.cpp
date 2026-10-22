/*
 * SPDX-License-Identifier: MIT
 * Arduino Stream over an interrupt-driven Zephyr UART.
 */

#include "ZephyrUartStream.h"

#include <zephyr/drivers/uart.h>
#include <zephyr/sys/util.h>

bool ZephyrUartStream::begin(const struct device *dev, void (*on_rx)(void),
			     void (*on_tx_idle)(void))
{
	if (!device_is_ready(dev)) {
		return false;
	}
	_dev = dev;
	_on_rx = on_rx;
	_on_tx_idle = on_tx_idle;
	ring_buf_init(&_rx_ring, sizeof(_rx_data), _rx_data);
	ring_buf_init(&_tx_ring, sizeof(_tx_data), _tx_data);
	k_sem_init(&_tx_drained, 0, 1);
	uart_irq_callback_user_data_set(dev, isr, this);
	uart_irq_rx_enable(dev);
	return true;
}

void ZephyrUartStream::isr(const struct device *dev, void *user_data)
{
	ZephyrUartStream *self = static_cast<ZephyrUartStream *>(user_data);

	for (;;) {
		uart_irq_update(dev);
		if (uart_irq_is_pending(dev) <= 0) {
			break;
		}

		if (uart_irq_rx_ready(dev)) {
			uint8_t buf[64];
			uint32_t room = MIN(sizeof(buf), ring_buf_space_get(&self->_rx_ring));

			if (room == 0) {
				/* read() re-enables RX once it has made room. */
				atomic_set(&self->_rx_paused, 1);
				uart_irq_rx_disable(dev);
			} else {
				int n = uart_fifo_read(dev, buf, room);

				if (n > 0) {
					ring_buf_put(&self->_rx_ring, buf, n);
				}
			}
			if (self->_on_rx) {
				self->_on_rx();
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *out;
			bool empty;
			k_spinlock_key_t key = k_spin_lock(&self->_tx_lock);
			uint32_t claimed = MIN(ring_buf_get_ptr(&self->_tx_ring, &out, 0), 64U);

			if (claimed > 0) {
				int sent = uart_fifo_fill(dev, out, claimed);

				if (sent > 0) {
					ring_buf_consume(&self->_tx_ring, sent);
				}
			}
			empty = ring_buf_is_empty(&self->_tx_ring);
			if (empty) {
				/* Inside the lock: write() enables after its put. */
				uart_irq_tx_disable(dev);
			}
			k_spin_unlock(&self->_tx_lock, key);

			if (empty) {
				k_sem_give(&self->_tx_drained);
				if (self->_on_tx_idle) {
					self->_on_tx_idle();
				}
			}
		}
	}
}

size_t ZephyrUartStream::write(const uint8_t *buffer, size_t size)
{
	if (!_dev || size == 0) {
		return 0;
	}
	k_spinlock_key_t key = k_spin_lock(&_tx_lock);
	uint32_t n = ring_buf_put(&_tx_ring, buffer, size);

	k_spin_unlock(&_tx_lock, key);
	if (n > 0) {
		uart_irq_tx_enable(_dev);
	}
	return n;
}

int ZephyrUartStream::availableForWrite()
{
	if (!_dev) {
		return 0;
	}
	k_spinlock_key_t key = k_spin_lock(&_tx_lock);
	uint32_t space = ring_buf_space_get(&_tx_ring);

	k_spin_unlock(&_tx_lock, key);
	return (int)space;
}

int ZephyrUartStream::available()
{
	return _dev ? (int)ring_buf_size_get(&_rx_ring) : 0;
}

int ZephyrUartStream::read()
{
	uint8_t b;

	if (!_dev || ring_buf_get(&_rx_ring, &b, 1) != 1) {
		return -1;
	}
	if (atomic_cas(&_rx_paused, 1, 0)) {
		uart_irq_rx_enable(_dev);
	}
	return b;
}

int ZephyrUartStream::peek()
{
	uint8_t b;

	if (!_dev || ring_buf_peek(&_rx_ring, &b, 1) != 1) {
		return -1;
	}
	return b;
}

void ZephyrUartStream::flush()
{
	if (!_dev) {
		return;
	}
	k_sem_reset(&_tx_drained);
	k_spinlock_key_t key = k_spin_lock(&_tx_lock);
	bool empty = ring_buf_is_empty(&_tx_ring);

	k_spin_unlock(&_tx_lock, key);
	if (!empty) {
		k_sem_take(&_tx_drained, K_MSEC(FLUSH_TIMEOUT_MS));
	}
}
