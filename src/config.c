#include "aicli.h"
#include "config.h"

#include <stdlib.h>

bool aicli_config_load_from_env(aicli_config_t *out) {
	if (!out) return false;
	out->openai_api_key = getenv("OPENAI_API_KEY");
	out->openai_base_url = getenv("OPENAI_BASE_URL");
	out->model = getenv("AICLI_MODEL");
	out->brave_api_key = getenv("BRAVE_API_KEY");
	return true;
}
