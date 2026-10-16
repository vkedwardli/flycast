#include "hw/pvr/ta.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/Renderer_if.h"

struct norend : Renderer
{
	bool Init() override {
		return true;
	}
	void Term() override { }

	void Process(TA_context* ctx) override {
		rendContext = &ctx->rend;
		ta_parse(ctx, true);
	}

	bool Render() override {
		// gdxsv: present once per rendered frame, as the real renderers do; a second
		// Present (frame swap after an immediate one) would pace headless runs at half speed.
		if (!rendContext->isRTT)
			frameRendered = true;
		return !rendContext->isRTT;
	}
	void RenderFramebuffer(const FramebufferInfo& info) override {
		rendContext = nullptr;
		frameRendered = true; // gdxsv: a framebuffer frame is presented once too
	}
	bool Present() override
	{
		if (!frameRendered)
			return false;
		frameRendered = false;
		return true;
	}

	rend_context *rendContext;
	bool frameRendered = false; // gdxsv: Render or RenderFramebuffer since the last Present
};

Renderer *rend_norend() {
	return new norend();
}
