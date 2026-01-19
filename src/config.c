#include "aicli.h"
#include "aicli_config.h"

#include <stdlib.h>
#include <string.h>

bool aicli_config_load_from_env(aicli_config_t *out)
{
	if (!out)
		return false;
	out->openai_api_key = getenv("OPENAI_API_KEY");
	out->openai_base_url = getenv("OPENAI_BASE_URL");
	out->model = getenv("AICLI_MODEL");
	out->debug_api = 0;
	out->debug_function_call = 0;

	// Search provider (default: Google CSE)
	out->search_provider = AICLI_SEARCH_PROVIDER_GOOGLE_CSE;
	{
		const char *p = getenv("AICLI_SEARCH_PROVIDER");
		if (p && p[0]) {
			if (strcmp(p, "google") == 0 || strcmp(p, "google_cse") == 0) {
				out->search_provider = AICLI_SEARCH_PROVIDER_GOOGLE_CSE;
			} else if (strcmp(p, "brave") == 0) {
				out->search_provider = AICLI_SEARCH_PROVIDER_BRAVE;
			}
		}
	}

	out->google_api_key = getenv("GOOGLE_API_KEY");
	out->google_cse_cx = getenv("GOOGLE_CSE_CX");
	out->brave_api_key = getenv("BRAVE_API_KEY");
	return true;
}
