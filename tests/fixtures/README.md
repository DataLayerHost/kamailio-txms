# Synthetic SIP fixtures

`.sip` files use CRLF framing and byte-accurate Content-Length. Replace addressing and Via headers before sending to a server. `sms-deliver.tpdu` is a binary first segment (DCS UTF-16BE, 8-bit reference 42, 2 parts) for `sms_framing=0`, not a full SIP request. Tests construct matching later segments dynamically.
