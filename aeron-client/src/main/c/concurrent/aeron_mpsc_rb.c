/*
 * Copyright 2014-2020 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include "aeron_mpsc_rb.h"
#include "util/aeron_error.h"

int aeron_mpsc_rb_init(volatile aeron_mpsc_rb_t *ring_buffer, void *buffer, size_t length)
{
    const size_t capacity = length - AERON_RB_TRAILER_LENGTH;
    int result = -1;

    if (AERON_RB_IS_CAPACITY_VALID(capacity))
    {
        ring_buffer->buffer = buffer;
        ring_buffer->capacity = capacity;
        ring_buffer->descriptor = (aeron_rb_descriptor_t *)(ring_buffer->buffer + ring_buffer->capacity);
        ring_buffer->max_message_length = AERON_RB_MAX_MESSAGE_LENGTH(ring_buffer->capacity);
        result = 0;
    }
    else
    {
        aeron_set_err(EINVAL, "%s:%d: %s", __FILE__, __LINE__, strerror(EINVAL));
    }

    return result;
}

inline static int32_t aeron_mpsc_rb_claim_capacity(volatile aeron_mpsc_rb_t *ring_buffer, size_t required_capacity)
{
    const size_t mask = ring_buffer->capacity - 1;
    int64_t head = 0;
    int64_t tail = 0;
    size_t tail_index = 0;
    size_t padding = 0;
    size_t to_buffer_end_length = 0;

    AERON_GET_VOLATILE(head, ring_buffer->descriptor->head_cache_position);

    do
    {
        int32_t available_capacity = 0;
        AERON_GET_VOLATILE(tail, ring_buffer->descriptor->tail_position);

        available_capacity = (int32_t)ring_buffer->capacity - (int32_t)(tail - head);

        if ((int32_t)required_capacity > available_capacity)
        {
            AERON_GET_VOLATILE(head, ring_buffer->descriptor->head_position);

            if (required_capacity > (ring_buffer->capacity - (int32_t)(tail - head)))
            {
                return -1;
            }

            AERON_PUT_ORDERED(ring_buffer->descriptor->head_cache_position, head);
        }

        padding = 0;
        tail_index = (int32_t)tail & mask;
        to_buffer_end_length = ring_buffer->capacity - tail_index;

        if (required_capacity > to_buffer_end_length)
        {
            size_t head_index = (int32_t)head & mask;

            if (required_capacity > head_index)
            {
                AERON_GET_VOLATILE(head, ring_buffer->descriptor->head_position);
                head_index = (int32_t)head & mask;

                if (required_capacity > head_index)
                {
                    return -1;
                }

                AERON_PUT_ORDERED(ring_buffer->descriptor->head_cache_position, head);
            }

            padding = to_buffer_end_length;
        }
    }
    while (!aeron_cmpxchg64(
        &(ring_buffer->descriptor->tail_position),
        tail,
        tail + (int32_t)required_capacity + (int32_t)padding));

    if (0 != padding)
    {
        aeron_rb_record_descriptor_t *record_header =
            (aeron_rb_record_descriptor_t *)(ring_buffer->buffer + tail_index);

        record_header->msg_type_id = AERON_RB_PADDING_MSG_TYPE_ID;
        AERON_PUT_ORDERED(record_header->length, (int32_t)padding);
        tail_index = 0;
    }

    return (int32_t)tail_index;
}

aeron_rb_write_result_t aeron_mpsc_rb_write(
    volatile aeron_mpsc_rb_t *ring_buffer,
    int32_t msg_type_id,
    const void *msg,
    size_t length)
{
    const size_t record_length = length + AERON_RB_RECORD_HEADER_LENGTH;
    const size_t required_capacity = AERON_ALIGN(record_length, AERON_RB_ALIGNMENT);
    int32_t record_index = 0;
    aeron_rb_write_result_t result = AERON_RB_FULL;

    if (length > ring_buffer->max_message_length || AERON_RB_INVALID_MSG_TYPE_ID(msg_type_id))
    {
        return AERON_RB_ERROR;
    }

    record_index = aeron_mpsc_rb_claim_capacity(ring_buffer, required_capacity);

    if (-1 != record_index)
    {
        aeron_rb_record_descriptor_t *record_header =
            (aeron_rb_record_descriptor_t *)(ring_buffer->buffer + record_index);
        record_header->msg_type_id = msg_type_id;
        AERON_PUT_ORDERED(record_header->length, -(int32_t)record_length);
        memcpy(ring_buffer->buffer + AERON_RB_MESSAGE_OFFSET(record_index), msg, length);
        AERON_PUT_ORDERED(record_header->length, (int32_t)record_length);

        result = AERON_RB_SUCCESS;
    }

    return result;
}

size_t aeron_mpsc_rb_read(
    volatile aeron_mpsc_rb_t *ring_buffer,
    aeron_rb_handler_t handler,
    void *clientd,
    size_t message_count_limit)
{
    const int64_t head = ring_buffer->descriptor->head_position;
    const size_t head_index = (int32_t)head & (ring_buffer->capacity - 1);
    const size_t contiguous_block_length = ring_buffer->capacity - head_index;
    size_t messages_read = 0;
    size_t bytes_read = 0;

    while ((bytes_read < contiguous_block_length) && (messages_read < message_count_limit))
    {
        aeron_rb_record_descriptor_t *header = NULL;
        const size_t record_index = head_index + bytes_read;
        int32_t record_length = 0;
        int32_t msg_type_id = 0;

        header = (aeron_rb_record_descriptor_t *)(ring_buffer->buffer + record_index);
        AERON_GET_VOLATILE(record_length, header->length);

        if (record_length <= 0)
        {
            break;
        }

        bytes_read += AERON_ALIGN(record_length, AERON_RB_ALIGNMENT);
        msg_type_id = header->msg_type_id;

        if (AERON_RB_PADDING_MSG_TYPE_ID == msg_type_id)
        {
            continue;
        }

        ++messages_read;
        handler(
            msg_type_id,
            ring_buffer->buffer + AERON_RB_MESSAGE_OFFSET(record_index),
            record_length - AERON_RB_RECORD_HEADER_LENGTH,
            clientd);

    }

    if (0 != bytes_read)
    {
        memset(ring_buffer->buffer + head_index, 0, bytes_read);
        AERON_PUT_ORDERED(ring_buffer->descriptor->head_position, head + bytes_read);
    }

    return messages_read;
}

int64_t aeron_mpsc_rb_next_correlation_id(volatile aeron_mpsc_rb_t *ring_buffer)
{
    int64_t result = 0;
    AERON_GET_AND_ADD_INT64(result, ring_buffer->descriptor->correlation_counter, 1);
    return result;
}

void aeron_mpsc_rb_consumer_heartbeat_time(volatile aeron_mpsc_rb_t *ring_buffer, int64_t now_ms)
{
    AERON_PUT_ORDERED(ring_buffer->descriptor->consumer_heartbeat, now_ms);
}

int64_t aeron_mpsc_rb_consumer_heartbeat_time_value(volatile aeron_mpsc_rb_t *ring_buffer)
{
    int64_t value = 0;
    AERON_GET_VOLATILE(value, ring_buffer->descriptor->consumer_heartbeat);
    return value;
}

inline static bool scan_back_to_confirm_still_zeroed(const uint8_t *buffer, size_t from, size_t limit)
{
    size_t i = from - AERON_RB_ALIGNMENT;
    bool all_zeroes = true;

    while (i >= limit)
    {
        const aeron_rb_record_descriptor_t *record = (aeron_rb_record_descriptor_t *)(buffer + i);
        int32_t length;
        AERON_GET_VOLATILE(length, record->length);
        if (0 != length)
        {
            all_zeroes = false;
            break;
        }

        i -= AERON_RB_ALIGNMENT;
    }

    return all_zeroes;
}

bool aeron_mpsc_rb_unblock(volatile aeron_mpsc_rb_t *ring_buffer)
{
    const size_t mask = ring_buffer->capacity - 1;
    int64_t head;
    int64_t tail;
    size_t consumer_index;
    size_t producer_index;
    bool unblocked = false;

    AERON_GET_VOLATILE(head, ring_buffer->descriptor->head_position);
    AERON_GET_VOLATILE(tail, ring_buffer->descriptor->tail_position);

    consumer_index = (int32_t)head & mask;
    producer_index = (int32_t)tail & mask;

    if (head == tail)
    {
        return false;
    }

    int32_t length;
    aeron_rb_record_descriptor_t *record = (aeron_rb_record_descriptor_t *)(ring_buffer->buffer + consumer_index);

    AERON_GET_VOLATILE(length, record->length);
    if (length < 0)
    {
        record->msg_type_id = AERON_RB_PADDING_MSG_TYPE_ID;
        AERON_PUT_ORDERED(record->length, -length);
        unblocked = true;
    }
    else if (0 == length)
    {
        const size_t limit = producer_index > consumer_index ? producer_index : ring_buffer->capacity;
        size_t i = consumer_index + AERON_RB_ALIGNMENT;

        do
        {
            record = (aeron_rb_record_descriptor_t *)(ring_buffer->buffer + i);
            AERON_GET_VOLATILE(length, record->length);
            if (0 != length)
            {
                if (scan_back_to_confirm_still_zeroed(ring_buffer->buffer, i, consumer_index))
                {
                    record = (aeron_rb_record_descriptor_t *)(ring_buffer->buffer + consumer_index);
                    record->msg_type_id = AERON_RB_PADDING_MSG_TYPE_ID;
                    AERON_PUT_ORDERED(record->length, (int32_t)(i - consumer_index));
                    unblocked = true;
                }

                break;
            }

            i += AERON_RB_ALIGNMENT;
        }
        while (i < limit);
    }

    return unblocked;
}

extern int64_t aeron_mpsc_rb_consumer_position(volatile aeron_mpsc_rb_t *ring_buffer);
extern int64_t aeron_mpsc_rb_producer_position(volatile aeron_mpsc_rb_t *ring_buffer);
