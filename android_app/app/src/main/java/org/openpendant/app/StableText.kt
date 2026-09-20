package org.openpendant.app

import android.widget.TextView

/** Polling must not reset unchanged text/layout/accessibility state. */
internal var TextView.stableText: CharSequence
    get() = text
    set(value) { if(text.toString()!=value.toString()) text=value }
