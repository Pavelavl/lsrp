# lsrp

## Request

- Bytes 0-3: Magic ("LSRP" в ASCII).
- Bytes 4-7: Длина параметров (uint32_t, big-endian).
- Bytes 8+: Параметры как строка (UTF-8), например: "file=example.rrd&start=now-1h&end=now&width=800&height=400".

Минимальный размер запроса: 8 байт + длина строки. Пример: для "file=test.rrd" — ~20 байт.

## Response

- Bytes 0-3: Magic ("LSRP" в ASCII).
- Byte 4: Статус (0 = OK, 1 = Error).
- Bytes 5-8: Длина данных (uint32_t, big-endian).
- Bytes 9+: Данные:

OK: SVG как строка (UTF-8).
 
Error: Короткое сообщение об ошибке (UTF-8, max 256 байт).
