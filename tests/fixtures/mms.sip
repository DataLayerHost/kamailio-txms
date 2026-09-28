MESSAGE sip:txms@example.invalid SIP/2.0
Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-example
From: <sip:sender@example.invalid>;tag=demo
To: <sip:txms@example.invalid>
Call-ID: fixture@example.invalid
CSeq: 1 MESSAGE
Max-Forwards: 70
Content-Type: multipart/mixed; boundary=demo
Content-Length: 114

--demo
Content-Type: text/plain
Content-Disposition: attachment; filename=example.txms.txt

0xabcd
--demo--
