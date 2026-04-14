#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static bool print(const char* data, size_t length) {
	const unsigned char* bytes = (const unsigned char*) data;
	for (size_t i = 0; i < length; i++)
		if (putchar(bytes[i]) == EOF)
			return false;
	return true;
}

/* Convert integer to string; returns number of characters written into buf.
 * Handles zero, positives, and negatives correctly.
 * buf must be at least 12 bytes (enough for "-2147483648"). */
static size_t format_int(int value, char* buf)
{
	if (value == 0) {
		buf[0] = '0';
		return 1;
	}

	size_t len = 0;

	if (value < 0) {
		buf[len++] = '-';
		/* Use unsigned arithmetic to avoid UB on INT_MIN */
		unsigned int uval = 0u - (unsigned int)value;
		char tmp[10];
		size_t ndigits = 0;
		while (uval > 0) {
			tmp[ndigits++] = '0' + (uval % 10);
			uval /= 10;
		}
		for (size_t i = ndigits; i > 0; i--)
			buf[len++] = tmp[i - 1];
	} else {
		unsigned int uval = (unsigned int)value;
		char tmp[10];
		size_t ndigits = 0;
		while (uval > 0) {
			tmp[ndigits++] = '0' + (uval % 10);
			uval /= 10;
		}
		for (size_t i = ndigits; i > 0; i--)
			buf[len++] = tmp[i - 1];
	}

	return len;
}
int printf(const char* restrict format, ...) {
	va_list parameters;
	va_start(parameters, format);

	int written = 0;

	while (*format != '\0') {
		size_t maxrem = INT_MAX - written;

		if (format[0] != '%' || format[1] == '%') {
			if (format[0] == '%')
				format++;
			size_t amount = 1;
			while (format[amount] && format[amount] != '%')
				amount++;
			if (maxrem < amount) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(format, amount))
				return -1;
			format += amount;
			written += amount;
			continue;
		}

		const char* format_begun_at = format++;

		if (*format == 'c') {
			format++;
			char c = (char) va_arg(parameters, int /* char promotes to int */);
			if (!maxrem) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(&c, sizeof(c)))
				return -1;
			written++;
		} else if (*format == 's') {
			format++;
			const char* str = va_arg(parameters, const char*);
			size_t len = strlen(str);
			if (maxrem < len) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(str, len))
				return -1;
			written += len;
		} else if (*format == 'd') {
			format++;
			int val = va_arg(parameters, int);
			char buf[12] = {0}; /* enough for "-2147483648" */
			size_t len = format_int(val, buf);
			if (maxrem < len) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(buf, len))
				return -1;
			written += len;
		} else if (*format == 'x') {
			format++;
			unsigned int val = va_arg(parameters, unsigned int);
			char buf[9] = {0}; /* 8 hex digits + null */
			size_t len = 0;
			if (val == 0) {
				buf[len++] = '0';
			} else {
				char tmp[8];
				size_t ndigits = 0;
				while (val > 0) {
					int digit = (int)(val & 0xF);
					tmp[ndigits++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
					val >>= 4;
				}
				for (size_t i = ndigits; i > 0; i--)
					buf[len++] = tmp[i - 1];
			}
			if (maxrem < len) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(buf, len))
				return -1;
			written += len;
		} else {
			format = format_begun_at;
			size_t len = strlen(format);
			if (maxrem < len) {
				// TODO: Set errno to EOVERFLOW.
				return -1;
			}
			if (!print(format, len))
				return -1;
			written += len;
			format += len;
		}
	}

	va_end(parameters);
	return written;
}
