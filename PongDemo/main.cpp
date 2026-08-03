
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <cstdint>
#include <cstdio>
#include <cwchar>       // wcscmp, wcstol
#include <TaskDAG.h>
#include <Window.h>
#include <Renderer2D.h>
#include <Helpers.h>
#include <Font.h>
#include <Camera2D.h>
#include <InputManager.h>
#include <memory>
#include <Colors.h>
#include <SceneManager.h>
#include <Time.h>
#include "GameplayScene.h"
//#include <InputManager.h>
// -w/--width, -h/--height, -warp/--warp. Parsed here (an app concern), then handed
// to the Window (size) and Renderer (warp adapter).
using namespace JLib;

static void ParseCommandLine(uint32_t& width, uint32_t& height, bool& useWarp)
{
	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) return;

	for (int i = 0; i < argc; ++i)
	{
		if ((wcscmp(argv[i], L"-w") == 0 || wcscmp(argv[i], L"--width") == 0) && i + 1 < argc)
			width = wcstol(argv[++i], nullptr, 10);
		else if ((wcscmp(argv[i], L"-h") == 0 || wcscmp(argv[i], L"--height") == 0) && i + 1 < argc)
			height = wcstol(argv[++i], nullptr, 10);
		else if (wcscmp(argv[i], L"-warp") == 0 || wcscmp(argv[i], L"--warp") == 0)
			useWarp = true;
	}
	LocalFree(argv);
}

// The whole program now reads as the lifecycle of two objects:
//   Window   -- owns the OS window + message pump (+ fullscreen/keys)
//   Renderer -- owns every GPU object + the render/resize/cleanup logic
int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	JLib::Time time;
	time.Initialize();
	JLib::TaskScheduler::Init();
	JLib::TaskScheduler& scheduler = JLib::TaskScheduler::Instance();

	// 100% client-area scaling, DPI-aware non-client chrome.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// WIC (used by DirectXTex's LoadFromWICFile to load textures) is COM-based and
	// requires a COM apartment on this thread; without this the texture load fails with
	// CO_E_NOTINITIALIZED. Initialize once for the app, uninitialize on exit.
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
		return -1;
	                           
	uint32_t width = 802, height = 455;
	bool useWarp = false;
	ParseCommandLine(width, height, useWarp);

	Window   window(hInstance, L"JLib Pong", width, height, false);
	// Query the REAL client area rather than trusting width/height blindly -- see Window's
	// constructor comment for why these can still differ by a pixel or two even after its own
	// internal correction pass.
	window.GetClientSize(width, height);
	// RendererCore: device/swap chain/fences/PRE-POST lists/particle system -- the low-level
	// GPU guts, reusable by a future 3D renderer. Renderer2D: sprite/text batching, driven BY
	// core's BeginFrame/PresentFrame via SetRenderer2D(). See both classes' header comments.
	RendererCore core;
	Renderer2D   renderer;
	core.SetRenderer2D(&renderer);
	window.SetRenderer(&core);                           // so WM_SIZE reaches Core (owns Resize/VSync)

	core.Initialize(window.GetHandle(), width, height, useWarp);
	renderer.Initialize(core); // builds root sig/PSO/instance buffer/SRV heap/font against core's device
	// Register particle effects AFTER both Initialize() calls (needs core's m_ComputeRootSignature/
	// m_CommandQueue), same pattern as RegisterEffect() for sprite shaders. Each gets its OWN
	// compute shader -- see UpdateParticles.hlsl (straight-line motion) and WaveParticles.hlsl
	// (sin-driven wave) -- and its own particle pool, so they can never step on each other's slots.
	// zLayer=1 here: draws over anything at zLayer 0 (e.g. background tiles) and under zLayer 2+
	// (e.g. the FPS/text overlay) -- see RendererCore::PresentFrame's layer-interleaving comment.
	uint32_t linearEffect = core.RegisterParticleEffect(L"shaders\\UpdateParticles.cso", 256000, false, 2,
		{}, 1, 1, { 1.0f, 0.1f, 0.1f, 0.0f });
	uint32_t waveEffect = core.RegisterParticleEffect(L"shaders\\WaveParticles.cso", 50000, false, 2,
		{}, 1, 1, { 0.2f, 0.6f, 1.0f, 0.0f });

	std::shared_ptr<InputManager> input;
	input = std::make_shared<InputManager>();
	// Raw Input registers against the real window (it must exist by now) and arrives as WM_INPUT;
	// the window procedure forwards those packets to the InputManager. Zero input threads.
	input->Initialize(window.GetHandle());
	Window::SetRawInputHandler(
		[](void* user, LPARAM lp) { static_cast<InputManager*>(user)->OnRawInput(lp); }, input.get());	window.Show();
	Vertex quadVertices[] = {
		{ -0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 0.0f }, // Top-Left
		{  0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 0.0f }, // Top-Right
		{ -0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f }, // Bottom-Left
		{  0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f }  // Bottom-Right
	};
	uint32_t quadIndices[] = { 0, 1, 2, 1, 3, 2 }; // The two-triangle quad
	auto resourceManager = renderer.GetResourceManager();
	// Flat white 1x1 texture for GameplayScene's tiles -- Resolve() intentionally throws on an
	// invalid/default-constructed TextureHandle (see ResourceManager.h's "fail loudly" comment,
	// not a bug), so tiles need a REAL handle; a solid white texture lets item.color do 100% of
	// the visual work with no wood/stone pattern showing through the tint.
	TextureHandle tileTexture;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		tileTexture = resourceManager->CreateSolidColorTexture(cmd, 255, 255, 255, 255);
		});
	// Same reasoning as tileTexture above -- renderer.GetCommandList() (the shared per-frame PRE
	// list) isn't a safe target for a one-off synchronous upload at setup time; it may not be
	// open, or may still be mid-flight from the previous frame. ExecuteUploadCommand wraps a
	// dedicated upload list (record, execute, wait) instead, same as every other texture load here.
	JLib::AssetHandle<JLib::AtlasResource> atlasHandle;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		atlasHandle = resourceManager->LoadAtlas(ExeRelative(L"textures\\sprites.atlas"), ExeRelative(L"textures\\sprites.png"), cmd);
		});
	// Same reasoning as tileTexture/atlasHandle above -- cmdList (renderer.GetCommandList(), the
	// shared per-frame PRE list) isn't safe to record a one-off setup-time upload into.
	TextureHandle background;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		background = resourceManager->LoadTexture(ExeRelative(L"textures\\Board.png"), cmd);
		});
//	Font font;                                             
 //	font.Load(ExeRelative(L"fonts\\Aldrich-Regular.fnt"), ExeRelative(L"fonts\\Aldrich-Regular.png"), renderer);
	// Meshes MUST be their own persistent variables, not stored by value inside a SpriteBatchItem --
	// Submit() caches a pointer to whatever Mesh an item points at, permanently, across every
	// future frame (see SpriteBatchItem::mesh's comment), so the Mesh needs to outlive the item.
	Mesh quadMesh = resourceManager->CreateMesh(quadVertices, 4, quadIndices, 6);
	DirectX::XMFLOAT4 clearColor = JLib::Colors::DarkBlue; // Define your background
	// GameplayScene's constructor sets position/zoom itself (world (0,0) = board center = screen
	// center) -- this default-constructed camera is just a placeholder until that runs.
	Camera2D camera;

	bool isRunning = true;
	SceneManager::PushScene(std::make_unique<GameplayScene>(*resourceManager, atlasHandle, time, renderer, input, camera, &quadMesh, tileTexture, width, height));

	while (window.ProcessMessages() && isRunning)
	{
		input->Update();
		// Horizontal move intent + jump are read here and consumed by the physics Update task
		// below (added to the per-frame DAG) -- gravity/collision/landing all come from
		// PhysicsSystem::ApplyPhysics now, not raw per-frame position deltas.

		float dt = renderer.GetFrameTime();
		renderer.UpdateGlobalUniforms(renderer.GetScreenSize(), camera.position, camera.zoom);

		// Per-frame DAG: StartFrame -> {sprites, text} (main-affinity, parallel-in-principle
		// but both actually run on main, serially, via ProcessMainThread) -> PresentFrame.
		// EVERY Submit-producer node MUST be added as a dependency of presentNode below, or
		// PresentFrame's FlushBatchParallel merge can race with a producer still submitting.
		JLib::TaskDAG dag(scheduler);

		core.m_StartFrameCtx = { &core, clearColor };
		auto* startTask = scheduler.CreateTask(RendererCore::StartFrameTask, &core.m_StartFrameCtx, false);
		auto* startNode = dag.CreateMainNode(startTask);

		// Physics step: SceneManager delegates to the current scene (GameplayScene), which owns
		// its own PhysicsWorld/SpatialGrid/player entirely -- must run before spritesNode reads
		// anything Draw() submits.
		auto* updateTask = scheduler.CreateTask([&isRunning, dt] {
			try {
				SceneManager::HandleInput(dt);
				SceneManager::Update(isRunning, dt);
			}
			catch (const std::exception& e) {
				FILE* f = nullptr;
				fopen_s(&f, "crash.log", "a");
				if (f) { fprintf(f, "std::exception in Update: %s\n", e.what()); fclose(f); }
				isRunning = false;
			}
			});
		auto* updateNode = dag.CreateMainNode(updateTask);
		dag.AddDependency(updateNode, startNode);

		auto* spritesTask = scheduler.CreateTask([&renderer, linearEffect, waveEffect, &quadMesh,background] {
			// The map/player/physics world itself -- GameplayScene::Draw() submits one BatchItem
			// per active PhysicsWorld cell.
			JLib::BatchItem item;
			item.mesh = &quadMesh;
			item.tex = background;
			item.position = { 802.0f/2.0f, 455.0f/2.0f };
			item.size = { 802.0f, 455.0f };
			item.color = { 1.0f, 1.0f, 1.0f, 1.0f };
			renderer.Submit(item);
			SceneManager::Draw();

			// effectID, basePosition, offsetRadius (jitter so multiple spawns scatter instead of
			// stacking), velocity, lifetime, color.
		//	renderer.RequestSpawn(linearEffect, { 300,300 }, 20.0f, { 0.0f, -50.0f }, 5.0f, { 1.0f, 0.6f, 0.1f, 1.0f });
			// velocity.y is repurposed as wave AMPLITUDE by WaveParticles.hlsl, not a real
			// vertical speed -- see that file's comment.
		//	renderer.RequestSpawn(waveEffect, { 0,0 }, 20.0f, { 30.0f, 60.0f }, 5.0f, { 0.2f, 0.6f, 1.0f, 1.0f });
			});
		auto* spritesNode = dag.CreateMainNode(spritesTask);
		dag.AddDependency(spritesNode, startNode);
		dag.AddDependency(spritesNode, updateNode);

		auto* textTask = scheduler.CreateTask([&renderer] {
			Font* f = renderer.GetSysFont();
			//	renderer.SubmitText(*f,  400.0f, 150.0f, "Left Aligned\ntest", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Left, 0);
			//	renderer.SubmitText(*f,  400.0f, 250.0f, "Center Aligned", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Center, 0);
			//	renderer.SubmitText(*f,  400.0f, 350.0f, "Right Aligned\ntest", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Right, 0);
			//renderer.UpdateFPS(); // its own SubmitText call -- same tier, same rules
			});
		auto* textNode = dag.CreateMainNode(textTask);
		dag.AddDependency(textNode, startNode);

		core.m_UpdateParticlesCtx = { &core };
		auto* particlesTask = scheduler.CreateTask(RendererCore::UpdateParticlesTask, &core.m_UpdateParticlesCtx);
		auto* particlesNode = dag.CreateMainNode(particlesTask);
		dag.AddDependency(particlesNode, startNode);

		core.m_PresentFrameCtx = { &core };
		auto* presentTask = scheduler.CreateTask(RendererCore::PresentFrameTask, &core.m_PresentFrameCtx);
		auto* presentNode = dag.CreateMainNode(presentTask);
		dag.AddDependency(presentNode, spritesNode);
		dag.AddDependency(presentNode, textNode);
		dag.AddDependency(presentNode, particlesNode);

		JLib::WaitGroup frameWg;
		frameWg.n.store(1, std::memory_order_relaxed); // 1 = the PresentFrame tail node
		presentTask->waitGroup = &frameWg;             // set BEFORE dag.Submit()

		dag.Submit();
		scheduler.WaitForMain(frameWg); // pumps ProcessMainThread; frame N+1 can't start until this returns

	}                               // continuous render loop
	// MUST be explicit, here, before renderer (Renderer2D) goes out of scope -- local
	// destruction order is REVERSE of declaration order, so renderer destructs BEFORE core
	// does. Relying on ~RendererCore() alone means Cleanup()'s GPU-idle Flush() would run
	// AFTER Renderer2D's command lists/allocators/instance buffer are already released,
	// letting the GPU still be mid-flight on resources that just got torn out from under it --
	// that's the shutdown freeze (driver doing extra recovery work), not a coincidence.
	core.Cleanup();
	// Same category of bug, different object: input's destructor doesn't run until this
	// function's closing brace, which is AFTER CoUninitialize() below -- releasing GameInput's
	// COM/WinRT object post-CoUninitialize is undefined behavior and fails fast. Must shut it
	// down explicitly first.
	Window::SetRawInputHandler(nullptr, nullptr);   // detach before the InputManager dies
	input->Shutdown();
	CoUninitialize();
	return 0;
}
