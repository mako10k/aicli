#include "aicli.h"
#include "aicli_config.h"

#include <stdlib.h>
#include <string.h>

void aicli_config_free(aicli_config_t *cfg)
{
	if (!cfg)
		return;
	// Only free heap-owned strings.
	if (cfg->openai_api_key_owned)
		free((void *)cfg->openai_api_key);
	if (cfg->openai_base_url_owned)
		free((void *)cfg->openai_base_url);
	if (cfg->model_owned)
		free((void *)cfg->model);
	if (cfg->google_api_key_owned)
		free((void *)cfg->google_api_key);
	if (cfg->google_cse_cx_owned)
		free((void *)cfg->google_cse_cx);
	if (cfg->brave_api_key_owned)
		free((void *)cfg->brave_api_key);
	memset(cfg, 0, sizeof(*cfg));
}

bool aicli_config_load_from_env(aicli_config_t *out)
{
	if (!out)
		return false;
	// Prefer OPENAI_API_KEY for backward compatibility.
	const char *k = getenv("OPENAI_API_KEY");
	if (!k || !k[0])
		k = getenv("AICLI_OPENAI_API_KEY");
	out->openai_api_key = k;
	out->openai_api_key_owned = false;
	out->openai_base_url = getenv("OPENAI_BASE_URL");
	out->openai_base_url_owned = false;
	out->model = getenv("AICLI_MODEL");
	out->model_owned = false;
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
	out->google_api_key_owned = false;
	out->google_cse_cx = getenv("GOOGLE_CSE_CX");
	out->google_cse_cx_owned = false;
	out->brave_api_key = getenv("BRAVE_API_KEY");
	out->brave_api_key_owned = false;
	return true;
}
