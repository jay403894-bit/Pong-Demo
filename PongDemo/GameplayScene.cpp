#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "GameplayScene.h"
#include "SceneManager.h"
#include <TaskScheduler.h>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <Helpers.h>
#include <Rect.h>

using namespace JLib;

GameplayScene::GameplayScene(ResourceManager& resourceManager, JLib::AssetHandle<AtlasResource>& atlas, Time& time, JLib::Renderer2D& renderer, std::shared_ptr<JLib::InputManager> input, JLib::Camera2D& camera,
	JLib::Mesh* quadMesh, JLib::TextureHandle tileTexture, uint32_t width, uint32_t height)
	: resourceManager(&resourceManager)
	, atlas(&atlas)
	, renderer(renderer)
	, time(&time)
	, input(input)
	, camera(camera)
	, quadMesh(quadMesh)
	, tileTexture(tileTexture)
	, width(width)
	, height(height)
{
	// WorldToScreen centers world (0,0) on the screen's middle -- with the camera parked there,
	// {0,0} maps to screen center. The board itself is defined below in board-local coordinates
	// centered on its OWN middle, so this camera position puts the board's center at the screen's
	// center, i.e. the whole board exactly fills the window.
	camera.position = { 0.0f, 0.0f };
	camera.zoom = 1.0f;
	auto screenSize = renderer.GetScreenSize();
	ball.screenSize = screenSize;
	ball.size = { 30.0f, 30.0f };
	ball.pos = { screenSize.x / 2,screenSize.y / 2 };
	ball.vel = { 300.0f,300.0f };
	player.pos.x = screenSize.x - player.size.x / 2 - 10.0f;
	player.pos.y = screenSize.y / 2;
	// px/sec, matching Ball::vel's units -- HandleInput multiplies by dt (see below), same as
	// CpuPaddle::Update already does. The old "6.0f, no dt" version moved the player paddle at
	// an effective 360 px/sec (6px * 60fps) while the AI's identical "6.0f * dt" moved it at
	// ~6 px/sec -- same field, same value, two different units, which is why the AI looked frozen.
	player.speed = 400.0f;
	ai.pos.x = 10.0f + ai.size.x / 2;
	ai.pos.y = screenSize.y / 2;
	ai.speed = 250.0f;
	ball.ai_score = &ai_score;
	ball.player_score = &player_score;
}

//gridOutlineEffectID = renderer.RegisterEffect(L"shaders\\VertexShader.cso", L"shaders\\GridOutlinePS.cso",
//	JLib::Renderer2D::DefaultAlphaBlend());
//if (!sound.Initialize(1)) {
//	throw("failed to initialize sound on thread 1");
//}

//music = sound.PlayLoop(JLib::ExeRelativeA("sound\\tomasz-kucza-chiptune-tchaikovsky.mp3").c_str());	
//	if (!music.IsValid()) {
//		throw("PlayLoop(\"music.mp3\") failed to load -- put a real file next to the exe to test.\n");
//	}


void GameplayScene::HandleInput(float dt)
{
	// Escape quits. This demo pushes GameplayScene as its ONLY scene, so there is nothing to pop
	// back to -- popping would just empty the stack and leave the app running with nothing drawn.
	// Update() is the only place with access to isRunning, hence the queued command.
	if (input->IsKeyPressed(VK_ESCAPE)) cmdQ.push_back(CMD::QUIT);

	// speed is px/sec now (matches Ball::vel's and CpuPaddle::Update's units) -- multiply by dt
	// so a held key moves the paddle at a constant real-world rate regardless of frame rate,
	// instead of "speed pixels every frame" (frame-rate DEPENDENT -- twice the FPS meant twice
	// the paddle speed under the old code).
	if (input->IsKeyDown(VK_UP)) {
		player.pos.y -= player.speed * dt;
	}
	if (input->IsKeyDown(VK_DOWN)) {
		player.pos.y += player.speed * dt;
	}

	// Clamp POSITION only -- speed is the paddle's input-driven move rate, not something that
	// should get zeroed by touching a wall. That was copy-pasted from Ball::Update's bounce
	// logic, where reversing/zeroing velocity makes sense for something that bounces; a paddle
	// isn't bouncing, it's directly re-driven by held input every frame, so touching the edge
	// should just stop further movement that frame via the position clamp -- zeroing speed
	// permanently disabled movement forever, since nothing else ever set it back to nonzero.
	// Also dropped the X-axis clamp entirely -- the paddle never moves horizontally (pos.x is
	// set once at construction and nothing here ever changes it), so it was dead weight copied
	// from the same source, not a real bound this paddle can ever actually hit.
	DirectX::XMFLOAT2 screenSize = renderer.GetScreenSize();
	float halfH = player.size.y * 0.5f;
	if (player.pos.y + halfH > screenSize.y) {
		player.pos.y = screenSize.y - halfH;
	}
	else if (player.pos.y - halfH < 0) {
		player.pos.y = halfH;
	}
}

GameplayScene::~GameplayScene()
{
	//sound.Shutdown();
}

void GameplayScene::DrawBall(const Ball& ball)
{
	const JLib::AtlasRegion& ballRegion = resourceManager->GetAtlasRegion(*atlas, "Ball");
	DirectX::XMFLOAT2 screenSize = renderer.GetScreenSize();
	BatchItem ballItem;
	ballItem.mesh = quadMesh;
	ballItem.tex = resourceManager->GetAtlasTexture(*atlas);  // the ONE shared atlas texture
	ballItem.uvOffset = ballRegion.uvOffset;                      // which sub-rect within it
	ballItem.uvScale = ballRegion.uvScale;
	ballItem.position = ball.pos;
	ballItem.size = ball.size;
	ballItem.color = { 1, 1, 1, 1 };
	renderer.Submit(ballItem);
}

void GameplayScene::DrawPlayerPaddle(const Paddle& paddle)
{
	auto screenSize = renderer.GetScreenSize();
	BatchItem player;
	player.mesh = quadMesh;
	const JLib::AtlasRegion& playerRegion = resourceManager->GetAtlasRegion(*atlas, "Player");
	player.tex = resourceManager->GetAtlasTexture(*atlas);  // the ONE shared atlas texture
	player.uvOffset = playerRegion.uvOffset;                      // which sub-rect within it
	player.uvScale = playerRegion.uvScale;
	player.size = paddle.size;
	player.position = paddle.pos;
	renderer.Submit(player);
}

void GameplayScene::DrawAIPaddle(const Paddle& paddle)
{
	auto screenSize = renderer.GetScreenSize();
	BatchItem ai;
	ai.mesh = quadMesh;
	const JLib::AtlasRegion& aiRegion = resourceManager->GetAtlasRegion(*atlas, "Computer");
	ai.tex = resourceManager->GetAtlasTexture(*atlas);  // the ONE shared atlas texture
	ai.uvOffset = aiRegion.uvOffset;                      // which sub-rect within it
	ai.uvScale = aiRegion.uvScale;
	ai.size = paddle.size;
	ai.position = paddle.pos;
	renderer.Submit(ai);
}



void GameplayScene::Update(bool& isRunning, float dt)
{
	for (CMD c : cmdQ) {
		if (c == CMD::QUIT) isRunning = false;
	}
	cmdQ.clear();
	if (!isRunning) return;

	ball.Update(dt);
	ai.Update(ball, dt, renderer.GetScreenSize());

	// Both Ball::pos/size and Paddle::pos/size are already CENTER + full-size, exactly
	// Physics2D::Rect's own convention (see Rect.h) -- no min-corner conversion needed anywhere.
	JLib::Rect ballRect({ ball.pos.x, ball.pos.y }, { ball.size.x, ball.size.y });
	JLib::Rect playerRect({ player.pos.x, player.pos.y }, { player.size.x, player.size.y });
	JLib::Rect aiRect({ ai.pos.x, ai.pos.y }, { ai.size.x, ai.size.y });

	// Same overshoot/double-flip risk Ball::Update's wall bounce had -- reversing vel.x alone
	// can leave the ball still overlapping the paddle next frame (fast ball, or a dt spike),
	// which would flip vel.x AGAIN before it ever cleared the paddle. Push the ball out along X
	// to the paddle's near edge in the same step as the flip, so it can never still be
	// overlapping on the next Update() call.
	// player is the RIGHT-side paddle (player.pos.x = screenSize.x - ...) -- the ball only ever
	// reaches it while moving RIGHT (vel.x > 0). ai is the LEFT-side paddle, reached while moving
	// LEFT (vel.x < 0). Gating on the wrong sign meant neither branch ever fired for the
	// direction the ball actually approaches from, so it just plowed through into the paddle
	// with no response at all -- only the far WALL (behind the paddle) ever bounced it, which is
	// what "stuck behind the paddle" was: bouncing back and forth in that gap, never against the
	// paddle itself.
	if (ball.vel.x > 0.0f && ballRect.Intersects(playerRect)) {
		ball.pos.x = player.pos.x - player.size.x * 0.5f - ball.size.x * 0.5f;
		ball.vel.x = -ball.vel.x;
	} else if (ball.vel.x < 0.0f && ballRect.Intersects(aiRect)) {
		ball.pos.x = ai.pos.x + ai.size.x * 0.5f + ball.size.x * 0.5f;
		ball.vel.x = -ball.vel.x;
	}
}



void GameplayScene::Draw()
{
	auto screenSize = renderer.GetScreenSize();
	DrawBall(ball);
	DrawPlayerPaddle(player);
	DrawAIPaddle(ai);
	renderer.SubmitText(*renderer.GetSysFont(),10.0f,10.0f, "Player Score: {}", 1.0f,JLib::Colors::Green,0.0f,JLib::TextAlign::Left,2,player_score);
	renderer.SubmitText(*renderer.GetSysFont(),10.0f,30.0f, "AI Score: {}", 1.0f,JLib::Colors::Red,0.0f,JLib::TextAlign::Left,2,ai_score);
}


