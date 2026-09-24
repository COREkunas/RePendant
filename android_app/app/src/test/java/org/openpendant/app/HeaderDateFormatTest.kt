package org.openpendant.app

import org.junit.Assert.*
import org.junit.Test
import java.time.Instant
import java.util.TimeZone

class HeaderDateFormatTest {
    private fun format(iso:String,zone:String="UTC")=HeaderDateFormat.format(Instant.parse(iso).toEpochMilli(),TimeZone.getTimeZone(zone))
    @Test fun exactFixedWidthCalendarDateAnd24HourTime() { assertEquals("2026-09-23 21:51",format("2026-09-23T21:51:59Z")) }
    @Test fun weekYearDoesNotLeakIntoCalendarYear() { assertEquals("2019-12-30 00:03",format("2019-12-30T00:03:00Z")) }
    @Test fun januaryUsesNewCalendarYear() { assertEquals("2021-01-01 00:00",format("2021-01-01T00:00:00Z")) }
    @Test fun phoneTimeZoneIsRespected() { assertEquals("2026-09-23 21:51",format("2026-09-23T18:51:00Z","Europe/Vilnius")) }
    @Test fun leapDayAndLeadingZeroes() { assertEquals("2024-02-29 01:02",format("2024-02-29T01:02:00Z")) }
}
