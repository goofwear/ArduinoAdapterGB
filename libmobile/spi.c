#include "spi.h"

#include "mobile.h"
#include "commands.h"

void mobile_spi_reset(struct mobile_adapter *adapter)
{
    adapter->spi.current = 0;
    adapter->spi.state = MOBILE_SPI_WAITING;
}

unsigned char mobile_transfer(struct mobile_adapter *adapter, unsigned char c)
{
    struct mobile_adapter_spi *s = &adapter->spi;

    mobile_board_time_latch();

    switch (s->state) {
    case MOBILE_SPI_WAITING:
        // Wait for the bytes that indicate a packet will be sent.
        if (c == 0x99) {
            s->current = 1;
        } else if (c == 0x66 && s->current == 1) {
            // Initialize transfer state
            s->data_size = 0;
            s->checksum = 0;
            s->error = 0;

            s->current = 0;
            s->state = MOBILE_SPI_DATA;
        } else {
            s->current = 0;
        }
        break;

    case MOBILE_SPI_DATA:
        // Receive the header and data. Calculate the checksum while at it.
        s->buffer[s->current++] = c;
        s->checksum += c;
        if (s->current == 4) {
            // Done receiving the header, read content size.
            s->data_size = s->buffer[3];

            // If we haven't begun a session, this is as good as any place to
            //   stop parsing, as we shouldn't react to this.
            if (!adapter->commands.session_begun &&
                    s->buffer[0] != MOBILE_COMMAND_BEGIN_SESSION) {
                mobile_spi_reset(adapter);
            }
        } else if (s->current >= s->data_size + 4) {
            s->state = MOBILE_SPI_CHECKSUM;
        }
        break;

    case MOBILE_SPI_CHECKSUM:
        // Receive the checksum, verify it when done.
        s->buffer[s->current++] = c;
        if (s->current >= s->data_size + 6) {
            uint16_t in_checksum = s->buffer[s->current - 2] << 8 |
                                   s->buffer[s->current - 1];
            if (s->checksum != in_checksum) {
                s->error = MOBILE_SPI_ERROR_CHECKSUM;
            }
            s->state = MOBILE_SPI_ACKNOWLEDGE;
            return adapter->device | 0x80;
        }
        break;

    case MOBILE_SPI_ACKNOWLEDGE:
        // Receive the acknowledgement byte, send error if applicable.
        if (c != (MOBILE_ADAPTER_GAMEBOY | 0x80)) {
            s->error = MOBILE_SPI_ERROR_UNKNOWN;
        }
        if (s->error) {
            mobile_spi_reset(adapter);
            return s->error;
        }

        s->retries = 0;
        s->current = 0;
        s->state = MOBILE_SPI_RESPONSE_WAITING;
        return s->buffer[0] ^ 0x80;

    case MOBILE_SPI_RESPONSE_WAITING:
        // Wait while processing the received packet and crafting a response.
        break;

    case MOBILE_SPI_RESPONSE_START:
        // Start sending the response.
        if (s->current++ == 0) {
            return 0x99;
        } else {
            s->data_size = s->buffer[3];
            s->current = 0;
            s->state = MOBILE_SPI_RESPONSE_DATA;
            return 0x66;
        }

    case MOBILE_SPI_RESPONSE_DATA:
        // Send all that's in the response buffer.
        // This includes the header, content and the checksum.
        c = s->buffer[s->current++];
        if (s->current > s->data_size + 6) {
            s->current = 0;
            s->state = MOBILE_SPI_RESPONSE_ACKNOWLEDGE;
            return adapter->device | 0x80;
        }
        return c;

    case MOBILE_SPI_RESPONSE_ACKNOWLEDGE:
        if (s->current++ == 0) {
            // There's nothing we can do with the received device ID.
            // In fact, the real adapter doesn't care for this value, either.
            return 0;
        } else {
            // TODO: Actually parse the error code.
            if ((c ^ 0x80) != s->buffer[0]) {
                // Retry sending the packet up to four times,
                //   if the checksum failed.
                if (++s->retries < 4) {
                    s->current = 1;
                    s->state = MOBILE_SPI_RESPONSE_START;
                    return 0x99;
                }
            }

            mobile_spi_reset(adapter);
        }
    }

    return 0xD2;
}
