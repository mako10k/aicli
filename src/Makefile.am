bin_PROGRAMS = aicli

aicli_SOURCES = \
	main.c \
	cli.c \
	config.c \
	openai_responses.c \
	brave_search.c \
	../vendor/yyjson/yyjson.c \
	buf.c \
	execute_dsl.c \
	execute/allowlist.c \
	execute/dispatch.c \
	execute/file_reader.c \
	execute/run_from_file.c \
	execute/paging.c \
	execute/pipeline_stages.c \
	execute_tool_impl.c \
	path_util.c \
	paging_cache.c

aicli_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/vendor/yyjson $(CURL_CFLAGS)

aicli_LDADD = $(CURL_LIBS)
