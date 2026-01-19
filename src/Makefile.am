bin_PROGRAMS = aicli

aicli_SOURCES = \
	main.c \
	cli.c \
	config.c \
	brave_search.c \
	buf.c \
	execute_dsl.c \
	execute/pipeline_stages.c \
	execute_tool_impl.c \
	path_util.c \
	paging_cache.c

aicli_CPPFLAGS = -I$(top_srcdir)/include $(CURL_CFLAGS)

aicli_LDADD = $(CURL_LIBS)
