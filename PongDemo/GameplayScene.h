#pragma once
#include <vector>
#include <memory>
#include <DirectXMath.h>
#include <Renderer2D.h>
#include <Camera2D.h>
#include <InputManager.h>
#include <GetTime.h>
#include <SoundManager.h>
#include <random>
#include "Scene.h"

// Ported from the source project's Level1 -- owns its own PhysicsWorld/SpatialGrid built from
// an inline map, and drives HandleInput/Update/Draw through SceneManager instead of being
// hardcoded into main()'s loop.
class GameplayScene : public Scene
{
public:
	GameplayScene(JLib::ResourceManager& resourceManager, JLib::AssetHandle<JLib::AtlasResource>& atlas, JLib::Time& time, JLib::Renderer2D& renderer, std::shared_ptr<JLib::InputManager> input, JLib::Camera2D& camera,
		JLib::Mesh* quadMesh, JLib::TextureHandle tileTexture, uint32_t width, uint32_t height);

	void Update(bool& isRunning, float dt = 0.0f) override;
	void Draw() override;
	void HandleInput(float dt) override;
	~GameplayScene() override;

private:
	struct Ball {
		DirectX::XMFLOAT2 screenSize;
		DirectX::XMFLOAT2 pos;
		DirectX::XMFLOAT2 vel;
		DirectX::XMFLOAT2 size{ 30.0f, 30.0f };
		int* player_score = nullptr;
		int* ai_score = nullptr;
		void Update(float dt) {
			pos.x += vel.x * dt;
			pos.y += vel.y * dt;
			// DrawBall assigns ball.pos straight to BatchItem::position with no transform, and
			// this engine's quad mesh spans -0.5..+0.5 -- item.position is always the sprite's
			// CENTER, not its top-left corner. Bouncing on "pos + size > screenSize" / "pos < 0"
			// (the raylib-tutorial top-left convention) was asymmetric here: it reflected size.x/y
			// (a full ball-width/-height) too early off the right/bottom edge, and let the ball
			// visually clip past the left/top edge by half its size before reflecting. Using
			// half-size on both sides makes it check the ball's ACTUAL rendered edges.
			float halfW = size.x * 0.5f;
			float halfH = size.y * 0.5f;
			// Reversing velocity alone isn't enough -- if the ball overshoots the edge in a
			// single step (fast vel, or a large dt spike), pos stays OUTSIDE the valid range
			// after the flip. Next frame the check is still true (pos is still past the edge),
			// so it flips again -- and since nothing ever pulled pos back in, it can flip-flop
			// while still drifting further out, walking off-screen instead of settling back in.
			// Clamping pos to the boundary alongside the velocity flip guarantees it can never
			// end up outside [halfExtent, screenSize - halfExtent], however far it overshot.
			if (pos.y + halfH > screenSize.y) {
				pos.y = screenSize.y - halfH;
				vel.y = -vel.y;
			} else if (pos.y - halfH < 0) {
				pos.y = halfH;
				vel.y = -vel.y;
			}
			if (pos.x + halfW > screenSize.x) {
				pos.x = screenSize.x - halfW;
				vel.x = -vel.x;
				// ai_score/player_score are int* -- "ai_score++" increments the POINTER (moves
				// it to point at whatever memory happens to follow, not the int it points to).
				// "(*ai_score)++" dereferences first, THEN increments the actual score value.
				if (ai_score) (*ai_score)++;
				Reset();
			} else if (pos.x - halfW < 0) {
				pos.x = halfW;
				vel.x = -vel.x;
				if (player_score) (*player_score)++;
				Reset();
			}
		}
		void Reset() {
			pos = { screenSize.x / 2, screenSize.y / 2 };
			// static: seeded ONCE (random_device is often a real OS syscall, and mt19937's
			// ~2.5KB state isn't free to construct either), not rebuilt every Reset() call --
			// which happens every time a point is scored. Function-local static init is
			// thread-safe/lazy by C++11 rules, so this is still safe even though Reset() could
			// in principle be called from more than one place.
			static std::mt19937 gen(std::random_device{}());
			static std::uniform_int_distribution<> distrib(0, 1);
			vel.x = (distrib(gen) == 0) ? -300.0f : 300.0f;
			vel.y = (distrib(gen) == 0) ? -300.0f : 300.0f;
		}
	};

	struct Paddle {
		DirectX::XMFLOAT2 pos;
		DirectX::XMFLOAT2 size{ 17.0f, 120.0f };
		float speed = 0.0f;
	
	};
	struct CpuPaddle : public Paddle {
		void Update(const Ball& ball, float dt, DirectX::XMFLOAT2 screenSize) {
			// Simple AI: Move towards the ball's Y position
			if (ball.pos.y > pos.y + size.y * 0.5f) {
				pos.y += speed * dt;
				float halfH = size.y * 0.5f;
				if (pos.y + halfH > screenSize.y) {
					pos.y = screenSize.y - halfH;
				}
				else if (pos.y - halfH < 0) {
					pos.y = halfH;
				}
			}
			else if (ball.pos.y < pos.y - size.y * 0.5f) {
				pos.y -= speed * dt;
				float halfH = size.y * 0.5f;
				if (pos.y + halfH > screenSize.y) {
					pos.y = screenSize.y - halfH;
				}
				else if (pos.y - halfH < 0) {
					pos.y = halfH;
				}
			}
		}
	};
	void DrawBall(const Ball& ball);
	void DrawPlayerPaddle(const Paddle& paddle);
	void DrawAIPaddle(const Paddle& paddle);
	Paddle player;
	CpuPaddle ai;
	int player_score = 0;
	int ai_score = 0;
	Ball ball;
	enum class CMD { QUIT };
	JLib::Time* time;
	int score = 0;
	JLib::AssetHandle<JLib::AtlasResource>* atlas;
	JLib::ResourceManager* resourceManager;
	double lastUpdateTime = 0.0;
	bool isGameOver = false;
	JLib::SoundManager sound;
	JLib::SoundHandle music;
	std::vector<CMD> cmdQ;
	JLib::Renderer2D& renderer;
	std::shared_ptr<JLib::InputManager> input;
	JLib::Camera2D& camera;
	JLib::Mesh* quadMesh;
	JLib::TextureHandle tileTexture; // FlushBatchTask/ResourceManager::Resolve requires a valid
	                                 // handle -- a default-constructed "no texture" one throws
	                                 // instead of silently skipping, so every tile still needs
	                                 // a real texture; color still does the per-tileID tinting.
	uint32_t width, height;

	DirectX::XMFLOAT4 ColorForTileID(uint32_t tileID) const;
	// Shared by Draw() (background grid) and DrawBlock() (the falling piece) -- both need to turn
	// an ABSOLUTE (row, col) on the 20x10 board into a final screen position, and that math
	// (board-centered world space -> camera.WorldToScreen) shouldn't be duplicated in two places.
	// pixelOffset is an optional extra nudge in SCREEN pixels, applied after the camera transform
	// (e.g. DrawBlock's raylib-tutorial-style offsetX/offsetY inset) -- {0,0} for the plain grid.
	DirectX::XMFLOAT2 CellToScreen(int row, int col, DirectX::XMFLOAT2 pixelOffset = { 0.0f, 0.0f }) const;
};
