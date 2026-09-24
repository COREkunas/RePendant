package org.openpendant.app

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/** Calendar year (not week-year), fixed digits and 24-hour time in the phone's zone. */
internal object HeaderDateFormat {
    fun format(at:Long):String = format(at,TimeZone.getDefault())
    fun format(at:Long,zone:TimeZone):String = SimpleDateFormat("yyyy-MM-dd HH:mm",Locale.ROOT).apply {
        timeZone=zone
    }.format(Date(at))
}
