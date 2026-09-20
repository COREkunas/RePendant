package org.openpendant.app

/** Exact lowercase, two characters per unsigned byte. No locale/Formatter
 * allocation per byte: metadata roundtrips call this for every manifest hash.
 * Representation only; never skips validation, hashing or durability work. */
internal fun canonicalHex(bytes: ByteArray): String {
    require(bytes.size <= Int.MAX_VALUE / 2)
    val alphabet = "0123456789abcdef"
    val chars = CharArray(bytes.size * 2)
    for (index in bytes.indices) {
        val value = bytes[index].toInt() and 255
        chars[index * 2] = alphabet[value ushr 4]
        chars[index * 2 + 1] = alphabet[value and 15]
    }
    return String(chars)
}
