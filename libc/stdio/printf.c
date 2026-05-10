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

static bool print_pad(char ch, int n) {
	for (int i = 0; i < n; i++)
		if (putchar((unsigned char)ch) == EOF)
			return false;
	return true;
}

static size_t format_int(int value, char* buf)
{
	if (value == 0) { buf[0] = '0'; return 1; }
	size_t len = 0;
	if (value < 0) {
		buf[len++] = '-';
		unsigned int uval = 0u - (unsigned int)value;
		char tmp[10]; size_t nd = 0;
		while (uval > 0) { tmp[nd++] = '0' + (uval % 10); uval /= 10; }
		for (size_t i = nd; i > 0; i--) buf[len++] = tmp[i - 1];
	} else {
		unsigned int uval = (unsigned int)value;
		char tmp[10]; size_t nd = 0;
		while (uval > 0) { tmp[nd++] = '0' + (uval % 10); uval /= 10; }
		for (size_t i = nd; i > 0; i--) buf[len++] = tmp[i - 1];
	}
	return len;
}

static size_t format_uint(unsigned int val, char* buf)
{
	if (val == 0) { buf[0] = '0'; return 1; }
	char tmp[10]; size_t nd = 0;
	while (val > 0) { tmp[nd++] = '0' + (val % 10); val /= 10; }
	size_t len = 0;
	for (size_t i = nd; i > 0; i--) buf[len++] = tmp[i - 1];
	return len;
}

static size_t format_hex(unsigned int val, char* buf)
{
	if (val == 0) { buf[0] = '0'; return 1; }
	char tmp[8]; size_t nd = 0;
	while (val > 0) {
		int d = (int)(val & 0xF);
		tmp[nd++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		val >>= 4;
	}
	size_t len = 0;
	for (size_t i = nd; i > 0; i--) buf[len++] = tmp[i - 1];
	return len;
}

/* Emit a formatted number with optional zero/space padding. */
static int emit_padded(const char* buf, size_t len,
                       bool zero_pad, int min_width, int* written)
{
	if ((int)len < min_width) {
		int pad_n = min_width - (int)len;
		if (!print_pad(zero_pad ? '0' : ' ', pad_n)) return -1;
		*written += pad_n;
	}
	if (!print(buf, len)) return -1;
	*written += (int)len;
	return 0;
}

int printf(const char* restrict format, ...) {
	va_list parameters;
	va_start(parameters, format);

	int written = 0;

	while (*format != '\0') {
		size_t maxrem = (size_t)(INT_MAX - written);

		if (format[0] != '%' || format[1] == '%') {
			if (format[0] == '%') format++;
			size_t amount = 1;
			while (format[amount] && format[amount] != '%') amount++;
			if (maxrem < amount) return -1;
			if (!print(format, amount)) return -1;
			format  += amount;
			written += (int)amount;
			continue;
		}

		const char* spec_start = format;   /* points at '%' */
		format++;                          /* now at char after '%' */

		/* ── Flags ── */
		bool zero_pad = false;
		if (*format == '0') { zero_pad = true; format++; }

		/* ── Width ── */
		int min_width = 0;
		while (*format >= '0' && *format <= '9') {
			min_width = min_width * 10 + (*format - '0');
			format++;
		}

		/* ── Conversion ── */
		if (*format == 'c') {
			format++;
			char c = (char)va_arg(parameters, int);
			if (min_width > 1) {
				if (!print_pad(zero_pad ? '0' : ' ', min_width - 1)) return -1;
				written += min_width - 1;
			}
			if (!print(&c, 1)) return -1;
			written++;

		} else if (*format == 's') {
			format++;
			const char* str = va_arg(parameters, const char*);
			size_t len = strlen(str);
			if ((int)len < min_width) {
				int pad_n = min_width - (int)len;
				if (!print_pad(zero_pad ? '0' : ' ', pad_n)) return -1;
				written += pad_n;
			}
			if (!print(str, len)) return -1;
			written += (int)len;

		} else if (*format == 'd') {
			format++;
			int val = va_arg(parameters, int);
			char buf[12] = {0};
			size_t len = format_int(val, buf);
			/* For negative with zero-pad: "-0042" — emit '-' then pad. */
			if (zero_pad && buf[0] == '-' && (int)len < min_width) {
				int pad_n = min_width - (int)len;
				if (putchar('-') == EOF) return -1;
				written++;
				if (!print_pad('0', pad_n)) return -1;
				written += pad_n;
				if (!print(buf + 1, len - 1)) return -1;
				written += (int)len - 1;
			} else {
				if (emit_padded(buf, len, zero_pad, min_width, &written) < 0)
					return -1;
			}

		} else if (*format == 'u') {
			format++;
			unsigned int val = va_arg(parameters, unsigned int);
			char buf[11] = {0};
			size_t len = format_uint(val, buf);
			if (emit_padded(buf, len, zero_pad, min_width, &written) < 0)
				return -1;

		} else if (*format == 'x') {
			format++;
			unsigned int val = va_arg(parameters, unsigned int);
			char buf[9] = {0};
			size_t len = format_hex(val, buf);
			if (emit_padded(buf, len, zero_pad, min_width, &written) < 0)
				return -1;

		} else {
			/* Unknown specifier — emit the whole % ... char literally. */
			size_t len = (size_t)(format - spec_start + 1);
			if (maxrem < len) return -1;
			if (!print(spec_start, len)) return -1;
			written += (int)len;
			format++;
		}
	}

	va_end(parameters);
	return written;
}
