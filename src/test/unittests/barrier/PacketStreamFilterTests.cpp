/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Debauchee Open Source Group
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "barrier/PacketStreamFilter.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"

#include "test/global/gtest.h"
#include "test/global/gmock.h"

#include <vector>

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;

namespace {

class WriteRecorder {
public:
    void record(const void* buffer, UInt32 count)
    {
        const UInt8* bytes = static_cast<const UInt8*>(buffer);
        writes.push_back(std::vector<UInt8>(bytes, bytes + count));
    }

    std::vector<std::vector<UInt8>> writes;
};

}

TEST(PacketStreamFilterTests, write_smallMessage_singleWriteWithHeader)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    WriteRecorder recorder;

    ON_CALL(stream, write(_, _)).WillByDefault(Invoke(&recorder, &WriteRecorder::record));

    PacketStreamFilter filter(&events, &stream, false);

    const UInt8 payload[] = { 'D', 'M', 'M', 'V', 1, 2, 3, 4 };
    filter.write(payload, sizeof(payload));

    ASSERT_EQ(1u, recorder.writes.size());

    const std::vector<UInt8>& packet = recorder.writes[0];
    ASSERT_EQ(sizeof(payload) + 4, packet.size());
    EXPECT_EQ(0u, packet[0]);
    EXPECT_EQ(0u, packet[1]);
    EXPECT_EQ(0u, packet[2]);
    EXPECT_EQ(sizeof(payload), packet[3]);
    EXPECT_TRUE(std::equal(payload, payload + sizeof(payload), packet.begin() + 4));
}

TEST(PacketStreamFilterTests, write_largeMessage_headerAndPayloadWrittenSeparately)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    WriteRecorder recorder;

    ON_CALL(stream, write(_, _)).WillByDefault(Invoke(&recorder, &WriteRecorder::record));

    PacketStreamFilter filter(&events, &stream, false);

    std::vector<UInt8> payload(4096, 0x5a);
    filter.write(&payload[0], static_cast<UInt32>(payload.size()));

    ASSERT_EQ(2u, recorder.writes.size());
    ASSERT_EQ(4u, recorder.writes[0].size());
    EXPECT_EQ(0x10u, recorder.writes[0][2]);
    EXPECT_EQ(0u, recorder.writes[0][3]);
    EXPECT_EQ(payload.size(), recorder.writes[1].size());
}
