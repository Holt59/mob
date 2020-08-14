#pragma once

#include "utility.h"
#include "env.h"
#include "context.h"

namespace mob
{

class url;


class async_pipe
{
public:
	async_pipe(const context& cx);

	handle_ptr create();
	std::string_view read(bool finish);
	bool closed() const;

private:
	static const std::size_t buffer_size = 50'000;

	const context& cx_;
	handle_ptr stdout_;
	handle_ptr event_;
	std::unique_ptr<char[]> buffer_;
	OVERLAPPED ov_;
	bool pending_;
	bool closed_;

	HANDLE create_pipe();
	std::string_view try_read();
	std::string_view check_pending();
};


class encoded_buffer
{
public:
	encoded_buffer(encodings e=encodings::dont_know, std::string bytes={});

	void add(std::string_view bytes);

	std::string utf8_string() const;

	template <class F>
	void next_utf8_lines(bool finished, F&& f)
	{
		for (;;)
		{
			switch (e_)
			{
				case encodings::utf16:
				{
					std::wstring_view utf16 =
						next_line<wchar_t>(finished, bytes_, last_);

					if (utf16.empty())
						return;

					f(utf16_to_utf8(utf16));
					break;
				}

				case encodings::acp:
				case encodings::oem:
				{
					std::string_view cp =
						next_line<char>(finished, bytes_, last_);

					if (cp.empty())
						return;

					f(bytes_to_utf8(e_, cp));
					break;
				}

				case encodings::utf8:
				case encodings::dont_know:
				default:
				{
					std::string_view utf8 =
						next_line<char>(finished, bytes_, last_);

					if (utf8.empty())
						return;

					f(std::string(utf8));
					break;
				}
			}
		}
	}

private:
	encodings e_;
	std::string bytes_;
	std::size_t last_;

	template <class CharT>
	std::basic_string_view<CharT> next_line(
		bool finished, std::string_view bytes, std::size_t& byte_offset)
	{
		std::size_t size = bytes.size();

		if constexpr (sizeof(CharT) == 2)
		{
			if ((size & 1) == 1)
				--size;
		}

		const CharT* start = reinterpret_cast<const CharT*>(bytes.data() + byte_offset);
		const CharT* end = reinterpret_cast<const CharT*>(bytes.data() + size);
		const CharT* p = start;

		std::basic_string_view<CharT> line;

		while (p != end)
		{
			if (*p == CharT('\n') || *p == CharT('\r'))
			{
				line = {start, static_cast<std::size_t>(p - start)};

				while (p != end && (*p == CharT('\n') || *p == CharT('\r')))
					++p;

				if (!line.empty())
					break;

				start = p;
			}
			else
			{
				++p;
			}
		}

		if (line.empty())
		{
			if (finished)
			{
				line = {
					reinterpret_cast<const CharT*>(bytes.data() + byte_offset),
					size - byte_offset
				};

				byte_offset = bytes.size();
			}
		}
		else
		{
			byte_offset = static_cast<std::size_t>(
				reinterpret_cast<const char*>(p) - bytes.data());

			MOB_ASSERT(byte_offset <= bytes.size());
		}

		return line;
	}
};


class process
{
public:
	enum flags_t
	{
		noflags                  = 0x00,
		allow_failure            = 0x01,
		terminate_on_interrupt   = 0x02,
		ignore_output_on_success = 0x04
	};

	enum arg_flags
	{
		noargflags      = 0x00,
		log_debug       = 0x01,
		log_trace       = 0x02,
		log_dump        = 0x04,
		log_quiet       = 0x08,
		nospace         = 0x10,
		quote           = 0x20,
		forward_slashes = 0x40
	};

	enum stream_flags
	{
		forward_to_log = 1,
		bit_bucket,
		keep_in_string,
		inherit
	};

	struct filter
	{
		std::string_view line;
		context::reason r;
		context::level lv;
		bool ignore;

		filter(
			std::string_view line, context::reason r, context::level lv,
			bool ignore)
				: line(line), r(r), lv(lv), ignore(ignore)
		{
		}


		// this is only used by filter_fun, where copying a `filter` wouldn't
		// make any sense
		filter(const filter&) = delete;
		filter& operator=(const filter&) = delete;
	};

	using filter_fun = std::function<void (filter&)>;


	process();
	~process();

	static process raw(const context& cx, const std::string& cmd);

	static process pipe(process p)
	{
		return p;
	}

	template <class... Processes>
	static process pipe(const process& p1, const process& p2, Processes&&... ps)
	{
		auto r = p1;
		r.pipe_into(p2);
		pipe(r, std::forward<Processes>(ps)...);
		return r;
	}

	process& set_context(const context* cx);

	process& name(const std::string& name);
	std::string name() const;

	process& binary(const fs::path& p);
	const fs::path& binary() const;

	process& cwd(const fs::path& p);
	const fs::path& cwd() const;

	process& stdout_flags(stream_flags s);
	process& stdout_level(context::level lv);
	process& stdout_filter(filter_fun f);
	process& stdout_encoding(encodings e);

	process& stderr_flags(stream_flags s);
	process& stderr_level(context::level lv);
	process& stderr_filter(filter_fun f);
	process& stderr_encoding(encodings e);

	process& chcp(int cp);
	process& cmd_unicode(bool b);

	process& external_error_log(const fs::path& p);

	process& flags(flags_t f);
	flags_t flags() const;

	process& success_exit_codes(std::set<int> v);

	template <class T, class=std::enable_if_t<!std::is_same_v<T, arg_flags>>>
	process& arg(const T& value, arg_flags f=noargflags)
	{
		add_arg("", arg_to_string(value, f), f);
		return *this;
	}

	template <class T, class=std::enable_if_t<!std::is_same_v<T, arg_flags>>>
	process& arg(const std::string& name, const T& value, arg_flags f=noargflags)
	{
		add_arg(name, arg_to_string(value, f), f);
		return *this;
	}

	template <template<class, class> class Container, class K, class V, class Alloc>
	process& args(const Container<std::pair<K, V>, Alloc>& v, arg_flags f=noargflags)
	{
		for (auto&& [name, value] : v)
			arg(name, value, f);

		return *this;
	}

	template <class Container>
	process& args(const Container& v, arg_flags f=noargflags)
	{
		for (auto&& e : v)
			add_arg(e, "", f);

		return *this;
	}

	process& env(const mob::env& e);

	void run();
	void interrupt();
	void join();

	int exit_code() const;
	std::string stdout_string();
	std::string stderr_string();

private:
	struct impl
	{
		handle_ptr handle;
		handle_ptr job;
		std::atomic<bool> interrupt{false};
		std::unique_ptr<async_pipe> stdout_pipe;
		std::unique_ptr<async_pipe> stderr_pipe;

		impl() = default;
		impl(const impl&);
		impl& operator=(const impl&);
	};

	struct stream
	{
		stream_flags flags = forward_to_log;
		context::level level = context::level::trace;
		filter_fun filter;
		encodings encoding = encodings::dont_know;
		encoded_buffer buffer;

		stream(context::level lv)
			: level(lv)
		{
		}
	};

	const context* cx_;
	std::string name_;
	fs::path bin_;
	fs::path cwd_;
	bool unicode_;
	int chcp_;
	flags_t flags_;
	std::set<int> success_;
	stream stdout_;
	stream stderr_;
	mob::env env_;
	std::string raw_;
	std::string cmd_;
	fs::path error_log_file_;
	std::map<context::level, std::vector<std::string>> logs_;

	impl impl_;
	DWORD code_;

	std::string make_name() const;
	std::string make_cmd() const;
	std::wstring make_cmd_args(const std::string& what) const;
	void pipe_into(const process& p);

	void do_run(const std::string& what);
	void read_pipes(bool finish);
	void read_pipe(bool finish, stream& s, async_pipe& pipe, context::reason r);

	void on_completed();
	void on_timeout(bool& already_interrupted);
	void terminate();

	void dump_error_log_file() noexcept;
	void dump_stderr() noexcept;

	void add_arg(const std::string& k, const std::string& v, arg_flags f);

	std::string arg_to_string(const char* s, arg_flags f);
	std::string arg_to_string(const std::string& s, arg_flags f);
	std::string arg_to_string(const fs::path& p, arg_flags f);
	std::string arg_to_string(const url& u, arg_flags f);
	std::string arg_to_string(int i, arg_flags f);
};


MOB_ENUM_OPERATORS(process::flags_t);

}	// namespace
