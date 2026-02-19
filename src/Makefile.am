bin_PROGRAMS = aicli

aicli_SOURCES = \
	main.c \
	cli.c \
	config.c \
	auto_search.c \
	openai_responses.c \
	brave_search.c \
	google_search.c \
	openai_tool_loop.c \
	threadpool.c \
	../vendor/yyjson/yyjson.c \
	buf.c \
	allowlist_list_tool.c \
	execute_dsl.c \
	execute/allowlist.c \
	execute/dispatch.c \
	execute/file_reader.c \
	execute/run_from_file.c \
	execute/paging.c \
	execute/pipeline_stages.c \
	execute_tool_impl.c \
	path_util.c \
	config_file.c \
	continue_state.c \
	paging_cache.c \
	md_render.c \
	web_tools.c \
	web_search_tool.c \
	web_fetch_tool.c

if HAVE_VENDORED_MD4C
aicli_SOURCES += ../vendor/md4c/src/md4c.c
endif

aicli_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/vendor/yyjson $(CURL_CFLAGS) $(MD4C_CFLAGS)

if HAVE_VENDORED_MD4C
aicli_CPPFLAGS += -I$(top_srcdir)/vendor/md4c/src
endif

if HAVE_MD4C
aicli_CPPFLAGS += -DHAVE_MD4C=1
endif

aicli_LDADD = $(CURL_LIBS) $(MD4C_LIBS)

aicli_CFLAGS = $(PTHREAD_CFLAGS)

aicli_LDADD += $(PTHREAD_LIBS)
