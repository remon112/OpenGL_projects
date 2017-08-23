/*!
  @file rx_fltk_glcanvas.cpp
	
  @brief FLTKによるOpenGLウィンドウクラス
 
  @author Makoto Fujisawa 
  @date   2011-09
*/
// FILE --rx_fltk_glcanvas.cpp--

#pragma warning (disable: 4996)
#pragma warning (disable: 4819)


//-----------------------------------------------------------------------------
// インクルードファイル
//-----------------------------------------------------------------------------
#include "rx_fltk_glcanvas.h"
#include "rx_fltk_window.h"

// テクスチャ・画像
#include "rx_texture.h"
#include "rx_bitmap.h"

// シミュレーション
#include "rx_sph.h"

// 設定ファイル
#include "rx_atom_ini.h"

// OpenGL描画関連
#include "rx_trackball.h"	// 視点変更用トラックボールクラス
#include "rx_shaders.h"		// GLSL関数


// CUDA
#include "rx_cu_funcs.cuh"

// メッシュ化
#include "rx_mesh.h"		// メッシュ構造体，クラス定義
#include "rx_mc.h"			// Marching Cubes


//-----------------------------------------------------------------------------
// 定数・変数
//-----------------------------------------------------------------------------
const uint NUM_PARTICLES = 50000;

const GLfloat RX_LIGHT0_POS[4] = {  0.0f, 1.0f, 0.0f, 1.0f };
const GLfloat RX_LIGHT1_POS[4] = { -1.0f, -10.0f, -1.0f, 0.0f };

const GLfloat RX_LIGHT_AMBI[4] = { 0.2f, 0.2f, 0.2f, 1.0f };
const GLfloat RX_LIGHT_DIFF[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
const GLfloat RX_LIGHT_SPEC[4] = { 0.4f, 0.4f, 0.4f, 1.0f };

const GLfloat RX_GREEN[] = { 0.1f, 1.0f, 0.1f, 1.0f };
const GLfloat RX_RED[]   = { 1.0f, 0.1f, 0.1f, 1.0f };
const GLfloat RX_BLUE[]  = { 0.1f, 0.1f, 1.0f, 1.0f };
const GLfloat RX_WHITE[] = { 1.0f, 1.0f, 1.0f, 1.0f };

const GLfloat RX_FOV = 45.0f;
const double RX_SPACE_INC = 0.002;

const int RX_SAVE_IMAGE_SPACING = 1;
const int RX_DISPLAY_STEP = 100;



// 
// externする必要がある変数など
//

// 設定ファイル
extern rxINI *g_pINI;

// 設定ファイルへの保存用
double g_fTBTran[3] = {0, 0, -5};	//!< 視点移動用トラックボールの平行移動量
double g_fTBQuat[4] = {1, 0, 0, 0};	//!< 視点移動用トラックボールの回転量
	
int g_iIterations = 1;				//!< 修正反復回数
double g_fEta = 0.0;				//!< 密度変動率

// 描画
rxGLSL g_glslPointSprite;			//!< GLSLを使った描画
rxGLSL g_glslFresnel;				//!< GLSLを使った描画
float g_fPScale = 1.0f;

// 時間計測
rxTimer g_TimerFPS;					//!< FPS測定用タイマー

// 平均計算時間計測
rxTimerAvg g_Time;

// レンダリング時の色設定用の値の範囲
double g_fMinValue = 0.0;
double g_fMaxValue = 1.0;

double g_fTotalTime;
double g_fAvgTime;
int g_iTimeCount;



//-----------------------------------------------------------------------------
// 関数プロトタイプ宣言
//-----------------------------------------------------------------------------
int DrawAxis(double len, double line_width = 5.0);
void DrawWireCuboid(const Vec3 &min, const Vec3 &max, const Vec3 &color);
void DrawStrings(vector<string> &static_str, int w, int h, int offsetx, int offsety);
void DrawStringsBottom(vector<string> &static_str, int w, int h, int offsetx, int offsety);

unsigned char* ReadImageFile(const std::string &fn, int &w, int &h, int &c);
int WriteImageFile(const std::string &fn, unsigned char *img, int w, int h, int c, int quality = 200);
bool SaveFrameBuffer(const string &fn, int w, int h);
bool SaveTexture(const string &fn, rxTexObj2D &tex_obj, int jpeg_quality = 200);
bool ReadTexture(const string &fn, rxTexObj2D &tex_obj);
bool LoadCubeMap(rxCubeMapData &cube_map, string base, string ext);




//-----------------------------------------------------------------------------
// rxFlWindowクラスの実装
//-----------------------------------------------------------------------------

//! コンストラクタ
rxFlGLWindow::rxFlGLWindow(int x_, int y_, int w_, int h_, const char* l, void *parent)
	: Fl_Gl_Window(x_, y_, w_, h_, l), m_iWinW(w_), m_iWinH(h_)
{
	m_pParent = (rxFlWindow*)parent;
	m_bAnimation = false;
	resizable(this);
	end();


	// 描画フラグ
	m_iDraw = 0;

	// クリップ平面
	m_fClipPlane[0] = 0.02;
	m_fClipPlane[1] = -1.0;

	// フォントサイズ
	m_ulFontSize = 14;

	m_iSelBufferSize = 1024;
	m_pSelBuffer = new GLuint[m_iSelBufferSize];


	//
	// シミュレーション用変数の初期化
	//
	m_pPS = 0;
	m_fDt = 0.005;
	m_fGravity = 9.80665;

	// アニメーションステップ
	m_iCurrentStep = 0;
	m_bPause = false;

	// シーンファイルの読み取り
	m_Scene.ReadSceneFiles();

	// シーンタイトルリスト
	m_vSceneTitles = m_Scene.GetSceneTitles();

	// 現在のシーン
	m_iCurrentSceneIdx = m_Scene.GetCurrentSceneIdx();

	m_iSimuSetting = 0;

	m_iSaveImageSpacing = -1;

	m_iColorType = RXP_COLOR_CONSTANT;
	m_fVScale = 0.02;

	// パーティクル情報出力
	m_strSphOutputName0  = RX_DEFAULT_DATA_DIR+"sph_setting.dat";
	m_strSphOutputHeader = RX_DEFAULT_DATA_DIR+"sph_particles_";

	// パーティクル情報入力
	m_strSphInputName0  = RX_DEFAULT_DATA_DIR+"sph_setting.dat";
	m_strSphInputHeader = RX_DEFAULT_DATA_DIR+"sph_particles_";

	//
	// メッシュ
	//
	m_iNumVrts = 0; m_iNumTris = 0;		// 生成されたメッシュの頂点数とメッシュ数
	m_iVertexStore = 5;					// サンプリングボリューム数に対する予想される頂点数(nx*ny*store)

	m_fMeshThr = 750.0;					// 陰関数メッシュ化時の閾値
	m_iMeshMaxN = 128;					// メッシュ化グリッド数(境界がもっとも長い軸方向の分割数)
	m_iMeshN[0] = 64;					// メッシュ化グリッド数
	m_iMeshN[1] = 64;
	m_iMeshN[2] = 64;

	m_vMeshBoundaryExt = Vec3(1.0);		// メッシュ境界ボックスの各辺の長さの1/2
	m_vMeshBoundaryCen = Vec3(0.0);		// メッシュ境界ボックスの中心座標

	m_iSolidDraw = 2;
	m_iLiquidDraw = 4;

	m_uVrtVBO = 0;
	m_uTriVBO = 0;
	m_uNrmVBO = 0;
	m_iDimVBO = 4;

	// メッシュ出力
	m_iSaveMeshSpacing = RX_SAVE_IMAGE_SPACING;

	// 背景画像
	m_bUseCubeMap = false;		// キューブマップ使用フラグ

	g_fTotalTime = 0;
	g_fAvgTime = 0;
	g_iTimeCount = 0;

	m_pMCMeshCPU = 0;
	m_pMCMeshGPU = 0;

	m_iPickedParticle = -1;
	
	MkDir("log");
	MkDir("result/data");
	MkDir("result/images");
	MkDir("result/mesh");


	// 設定ファイル
	if(g_pINI){
		//g_pINI->Set("gl", "draw", &m_iDraw, m_iDraw);
		g_pINI->Set("gl", "font", &m_ulFontSize, m_ulFontSize);

		g_pINI->Set("trackball", "tx",  &g_fTBTran[0],  0.0);
		g_pINI->Set("trackball", "ty",  &g_fTBTran[1],  0.0);
		g_pINI->Set("trackball", "tz",  &g_fTBTran[2], -4.0);
		g_pINI->Set("trackball", "q0",  &g_fTBQuat[0],  1.0);
		g_pINI->Set("trackball", "q1",  &g_fTBQuat[1],  0.0);
		g_pINI->Set("trackball", "q2",  &g_fTBQuat[2],  0.0);
		g_pINI->Set("trackball", "q3",  &g_fTBQuat[3],  0.0);

		g_pINI->Set("sph", "scene",  &m_iCurrentSceneIdx, -1);
		g_pINI->Set("sph", "mesh_threshold", &m_fMeshThr, m_fMeshThr);
		g_pINI->Set("sph", "setting", &m_iSimuSetting, 0);

		g_pINI->Set("sph", "solid_mesh", &m_iSolidDraw, m_iSolidDraw);
		g_pINI->Set("sph", "liquid_mesh", &m_iLiquidDraw, m_iLiquidDraw);
	}
}

//! デストラクタ
rxFlGLWindow::~rxFlGLWindow()
{
	if(m_pMCMeshGPU) delete m_pMCMeshGPU;
	if(m_pMCMeshCPU) delete m_pMCMeshCPU;

	if(m_uVrtVBO) glDeleteBuffers(1, &m_uVrtVBO);
	if(m_uTriVBO) glDeleteBuffers(1, &m_uTriVBO);
	if(m_uNrmVBO) glDeleteBuffers(1, &m_uNrmVBO);

	if(m_pPS) delete m_pPS;
}


/*! 
 * GLの初期化関数
 */
void rxFlGLWindow::InitGL(void)
{
	make_current();
	static int init = false;
	if(init) return;

	init = true;

	CuDeviceProp();
	CuSetDevice(0);

	RXCOUT << "OpenGL Ver. " << glGetString(GL_VERSION) << endl;

	GLenum err = glewInit();
	if(err == GLEW_OK){
		RXCOUT << "GLEW OK : Glew Ver. " << glewGetString(GLEW_VERSION) << endl;
	}
	else{
		RXCOUT << "GLEW Error : " << glewGetErrorString(err) << endl;
	}

	// OpenGL拡張のバージョンチェック
	if(!glewIsSupported("GL_VERSION_2_0 " 
						"GL_ARB_pixel_buffer_object "
						"GL_EXT_framebuffer_object "
						"GL_ARB_multitexture "
						"GL_ARB_vertex_buffer_object "
						)){
		RXCOUT << "ERROR: Support for necessary OpenGL extensions missing." << endl;
		exit(1);
	}

	GLint buf, sbuf;
	glGetIntegerv(GL_SAMPLE_BUFFERS, &buf);
	RXCOUT << "number of sample buffers is " << buf << endl;
	glGetIntegerv(GL_SAMPLES, &sbuf);
	RXCOUT << "number of samples is " << sbuf << endl;

	m_fBGColor[0] = 1.0; m_fBGColor[1] = 1.0; m_fBGColor[2] = 1.0;
	m_iMouseButton = 0;

	glClearColor((GLfloat)m_fBGColor[0], (GLfloat)m_fBGColor[1], (GLfloat)m_fBGColor[2], 1.0f);
	glClearDepth(1.0f);

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);

	glEnable(GL_AUTO_NORMAL);
	glEnable(GL_NORMALIZE);

	// 光源設定
	glEnable(GL_LIGHT0);
	glEnable(GL_LIGHTING);
	glLightfv(GL_LIGHT0, GL_POSITION, RX_LIGHT0_POS);
	glLightfv(GL_LIGHT0, GL_DIFFUSE,  RX_LIGHT_DIFF);
	glLightfv(GL_LIGHT0, GL_SPECULAR, RX_LIGHT_SPEC);
	glLightfv(GL_LIGHT0, GL_AMBIENT,  RX_LIGHT_AMBI);

	glShadeModel(GL_SMOOTH);

	glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
	glEnable(GL_COLOR_MATERIAL);

	// 視点初期化
	//InitView();

	// デフォルト材質
	m_matPoly.name      = "";
	m_matPoly.diffuse   = Vec4(0.2, 0.2, 1.0, 1.0);
	m_matPoly.specular  = Vec4(0.3, 0.3, 0.3, 1.0);
	m_matPoly.ambient   = Vec4(0.1, 0.1, 0.1, 1.0);
	m_matPoly.color     = Vec4(0.0);
	m_matPoly.emission  = Vec4(0.0);
	m_matPoly.shininess = 10.0;
	m_matPoly.illum = 2;
	m_matPoly.tex_file = "";
	m_matPoly.tex_name = 0;

	// キューブマップの読み込み
	if(LoadCubeMap(m_CubeMap, "texture/terragen0_", ".bmp")){
		m_bUseCubeMap = true;
	}
	else{
		RXCOUT << "error : can't load the cube map" << endl;
	}

	// トラックボール初期姿勢
	m_tbView.SetScaling(-5.0);

	m_tbView.SetTranslation(g_fTBTran[0], g_fTBTran[1]);
	m_tbView.SetScaling(g_fTBTran[2]);
	m_tbView.SetQuaternion(g_fTBQuat);

	m_iSimuSetting &= ~RX_SPH_INPUT;

	// 描画フラグ初期化
	m_iDraw = 0;
	m_iDraw |= RXD_BBOX;
	m_iDraw |= RXD_PARTICLE;
	m_iDraw |= RXD_SOLID;
	m_iDraw |= RXD_BPARTICLE_ENV;
	m_iDraw |= RXD_PARAMS;
	if(m_iSimuSetting & RX_SPH_MESH) m_iDraw |= RXD_MESH;

	m_iDrawPS = RXP_POINTSPRITE;


	// カレントのシーン設定
	m_Scene.SetCurrentScene(m_iCurrentSceneIdx);

	// SPH初期化
	InitSPH(m_Scene);
	
	// GLSLのコンパイル
	g_glslPointSprite = CreateGLSL(ps_vs, ps_fs, "point sprite");
	g_glslFresnel     = CreateGLSL(fresnel_vs, fresnel_fs, "fresnel");


	m_pParent->UpdateMenuState();
}

/*! 
 * リサイズイベント処理関数
 * @param[in] w キャンバス幅(ピクセル数)
 * @param[in] h キャンバス高さ(ピクセル数)
 */
void rxFlGLWindow::Resize(int w, int h)
{
	m_iWinW = w;
	m_iWinH = h;

	glViewport(0, 0, w, h);
	m_tbView.SetRegion(w, h);

	// 透視変換行列の設定
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	Projection();

	// モデルビュー変換行列の設定
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	string status_str = RX_TO_STRING(w)+" x "+RX_TO_STRING(h) + "  ";
	m_pParent->SetStatusLabel(status_str);
}

/*!
 * 透視投影変換
 */
void rxFlGLWindow::Projection(void)
{
	gluPerspective(RX_FOV, (float)m_iWinW/(float)m_iWinH, 0.01f, 100.0f);
	//glOrtho(-1, 1, -1, 1, -1, 1);
}

/*!
 * 再描画命令
 */
void rxFlGLWindow::ReDisplay(void)
{
	redraw();
}

/*!
 * 視点の初期化
 */
void rxFlGLWindow::InitView(void)
{
	double q[4] = {1, 0, 0, 0};
	m_tbView.SetQuaternion(q);
	m_tbView.SetScaling(-6.0);
	m_tbView.SetTranslation(0.0, 0.0);
}

/*!
 * 再描画イベント処理関数
 */
void rxFlGLWindow::Display(void)
{
	make_current();

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glViewport(0, 0, m_iWinW, m_iWinH);

	// 透視変換行列の設定
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	Projection();

	// モデルビュー行列初期化
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// キューブマップの描画
	if(m_bUseCubeMap && (m_iDraw & RXD_REFRAC)){
		glPushMatrix();
		glDisable(GL_CULL_FACE);
		//TrackballApplyTranslation();
		//TrackballApplyRotation();
		//DrawCubeMap(m_CubeMap, 100.0);
		glPopMatrix();
	}

	glPushMatrix();

	// マウスによる回転・平行移動の適用
	m_tbView.Apply();

	glPushMatrix();

	// シーン描画
	RenderSphScene();

	glPopMatrix();

	glPopMatrix();


	// 画面文字列描画
	if(m_iDraw & RXD_PARAMS){
		glColor3d(0.0, 0.0, 0.5);
		DrawStringsBottom(SetDrawString(), m_iWinW, m_iWinH, 0, 0);
	}

	// マウスピックされたパーティクルのパラメータ情報描画(デバッグ用)
	if((m_iDraw & RXD_PARAMS) && ((m_iPickedParticle != -1) || (m_pPS->GetNumParticles() == 1))){
		glColor3d(0.0, 0.0, 1.0);
		vector<string> strs;
		if(m_pPS->GetNumParticles() == 1){
			DivideString(m_pPS->GetParticleInfo(0), strs);
		}
		else{
			DivideString(m_pPS->GetParticleInfo(m_iPickedParticle), strs);
		}
		DrawStrings(strs, m_iWinW, m_iWinH, 0, 0);
	}

}

/*!
 * 再描画イベント処理関数(マウスピック用)
 */
void rxFlGLWindow::DisplayForPick_s(void* x)
{
	((rxFlGLWindow*)x)->DisplayForPick();
}
void rxFlGLWindow::DisplayForPick(void)
{
	make_current();

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glPushMatrix();
	// マウスによる回転・平行移動の適用
	m_tbView.Apply();

	glDisable(GL_LIGHTING);
	glColor4d(0.0, 0.0, 1.0, 1.0);
	//glEnable(GL_POINT_SMOOTH);
	glPointSize(8.0);

	int pnum = m_pPS->GetNumParticles();
	RXREAL *data = 0;
	data = m_pPS->GetArrayVBO(rxParticleSystemBase::RX_POSITION);
	int k = 0;
	for(int i = 0; i < pnum; ++i){
		glLoadName(i+1);
		glBegin(GL_POINTS);
		glVertex3d(data[k], data[k+1], data[k+2]);
		glEnd();

		k += DIM;
	}

	glPopMatrix();
}

/*!
 * 投影変換関数を外部から呼び出すためのstatic関数
 */
void rxFlGLWindow::Projection_s(void *x)
{
	((rxFlGLWindow*)x)->Projection();
}

/*!
 * マウスピックによるパーティクル選択
 * @param[in] x,y スクリーン上でのマウス座標
 */
bool rxFlGLWindow::PickParticle(int x, int y)
{
	vector<rxPickInfo> hits = pick(x, y, 16, 16);
	if(hits.empty()){
		m_iPickedParticle = -1;
		return false;
	}
	else{
		std::sort(hits.begin(), hits.end(), CompFuncPickInfo);
		m_iPickedParticle = hits[0].name-1;
		RXCOUT << "picked particle : " << m_iPickedParticle << endl;
		return true;
	}
}


/*!
	* OpenGLによるピックでヒットしたものから最小のデプス値を持つものを選択する
	* @param hits ヒット数
	* @param buf  選択バッファ
	* @return ヒットしたオブジェクト番号
	*/
vector<rxPickInfo> rxFlGLWindow::selectHits(GLint nhits, GLuint buf[])
{
	vector<rxPickInfo> hits;
 
	float depth_min = 100.0f;
	float depth1 = 1.0f;
	float depth2 = 1.0f;
 
	GLuint depth_name;
	GLuint *ptr;
		
	// ヒットしたデータなし
	if(nhits <= 0) return hits;
		
	hits.resize(nhits);

	// ポインタを作業用ptrへ渡す．
	ptr = (GLuint*)buf;
	for(int i = 0; i < nhits; ++i){
		// ヒットしたオブジェクトの名前
		depth_name = *ptr;
		ptr++;

		// ヒットしたプリミティブのデプス値の最小値
		depth1 = (float) *ptr/0x7fffffff;
		ptr++;

		// ヒットしたプリミティブのデプス値の最大
		depth2 = (float) *ptr/0x7fffffff;
		ptr++;

		hits[i].name = (int)(*ptr);
		hits[i].min_depth = depth1;
		hits[i].max_depth = depth2;

		ptr++;
	}

	return hits;
}
 
/*!
 * マウス選択
 * @param[in] x,y 選択中心座標(マウス座標系)
 * @param[in] w,h 選択範囲
 * @retval int ピックオブジェクト番号
 */
vector<rxPickInfo> rxFlGLWindow::pick(int x, int y, int w, int h)
{
	make_current();

	GLuint selbuf[256];
	GLint viewport[4];
 
	glGetIntegerv(GL_VIEWPORT, viewport);
	glSelectBuffer(1024, selbuf);
 
	glRenderMode(GL_SELECT);
 
	glInitNames();
	glPushName(0);	// スタック内に名前がひとつもないとLoadNameは働かないので0をプッシュしておく
 
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
 
	//gluPickMatrix(x, viewport[3]-y, w, h, viewport);
	gluPickMatrix(x, m_iWinH-y, w, h, viewport);
	Projection();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	DisplayForPick();
	glLoadName(0);
 
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glPopName();
 
	GLint nhits;
	nhits = glRenderMode(GL_RENDER);
		
	vector<rxPickInfo> hits = selectHits(nhits, selbuf);
	glMatrixMode(GL_MODELVIEW);

	return hits;
}


/*!
 * マウスイベント処理関数
 * @param[in] button マウスボタン(FL_LEFT_MOUSE,FL_MIDDLE_MOUSE,FL_RIGHT_MOUSE)
 * @param[in] state マウスボタンの状態(1:down, 0:up)
 * @param[in] x,y マウス座標(スクリーン座標系)
 */
void rxFlGLWindow::Mouse(int button, int state, int x, int y)
{
	make_current();
	m_iKeyMod = (Fl::event_state(FL_SHIFT) ? 2 : (Fl::event_state(FL_CTRL) ? 3 : 1));
	int mask = 0x01 << (button-1);
	m_iMouseButton = (state ? (m_iMouseButton | mask) : (m_iMouseButton & ~mask));

	if(button == FL_LEFT_MOUSE){
		if(state){	// ボタンダウン
			// マウスドラッグによる視点移動
			m_tbView.Start(x, y, m_iKeyMod);
		}
		else{		// ボタンアップ
			m_tbView.Stop(x, y);

			// 設定ファイルへの保存のために視点情報をトラックボールクラスから取り出しておく
			m_tbView.GetTranslation(g_fTBTran);
			m_tbView.GetScaling(g_fTBTran[2]);
			m_tbView.GetQuaternion(g_fTBQuat);
		}
	}
	else if(button == FL_RIGHT_MOUSE){
		if(state){	// ボタンダウン
			// マウスピック
			PickParticle(x, y);
		}
	}

	redraw();
}

/*!
 * モーションイベント処理関数(マウスボタンを押したままドラッグ)
 * @param[in] x,y マウス座標(スクリーン座標系)
 */
void rxFlGLWindow::Motion(int x, int y)
{
	if(x < 0 || y < 0) return;
	make_current();

	static int x0 = -1, y0 = -1;
	if(x0 == -1 || y0 == -1){
		x0 = x;
		y0 = y;
	}

	m_tbView.Motion(x, y);

	x0 = x;
	y0 = y;

	redraw();
}

/*!
 * モーションイベント処理関数(マウスボタンを押さない移動)
 * @param[in] x,y マウス座標(スクリーン座標系)
 */
void rxFlGLWindow::PassiveMotion(int x, int y)
{
	if(x < 0 || y < 0) return;
	//make_current();
	//redraw();
}

/*!
 * キーボードイベント処理関数
 *  - 描画領域にフォーカスがないとこの関数自体呼ばれない
 *  - ↑の理由からFLTKの場合は基本的にはメニュー項目にショートカットキーを当てることで代替
  * @param[in] key キーの種類
 * @param[in] x,y キーが押されたときのマウス座標(スクリーン座標系)
 */
void rxFlGLWindow::Keyboard(int key, int x, int y)
{
	make_current();
	m_iKeyMod = (Fl::event_state(FL_SHIFT) ? 2 : (Fl::event_state(FL_CTRL) ? 3 : 1));

	switch(key){
	case 'i':
		InitView();
		break;

	case 'S':	// 画面の画像保存
		SaveDisplay( ((m_iCurrentStep >= 0) ? m_iCurrentStep : 0) );
		break;

	case 'g':
		SwitchFullScreen();
		break;

	// 
	// シミュレーション設定
	// 
	case '*':
		AddSphere();
		break;

	case FL_Left:
	case FL_Right:
	case FL_Up:
	case FL_Down:
	case FL_Home:
	case FL_End:
	case FL_Page_Up:
	case FL_Page_Down:
	//case FL_Tab:
	case FL_BackSpace:
	case FL_Scroll_Lock:
	case FL_Delete:
		SpecialKey(key, x, y);
		return;

	default:
		break;
	}


	m_pParent->UpdateMenuState();
	ReDisplay();
}

/*!
 * 特殊キーボードイベント処理関数
 * @param[in] key キーの種類
 * @param[in] x,y キーが押されたときのマウス座標(スクリーン座標系)
 */
void rxFlGLWindow::SpecialKey(int key, int x, int y)
{
	if(m_iKeyMod == GLUT_ACTIVE_CTRL){
		if(key == FL_Left){
			RXCOUT << "clockwise" << endl;
			m_tbView.AddRotation(90, 0, 1, 0);
		}
		else if(key == FL_Right){
			RXCOUT << "counterclockwise" << endl;
			m_tbView.AddRotation(90, 0, -1, 0);
		}
		else if(key == FL_Up){
			RXCOUT << "up" << endl;
			m_tbView.AddRotation(90, 1, 0, 0);
		}
		else if(key == FL_Down){
			RXCOUT << "down" << endl;
			m_tbView.AddRotation(90, -1, 0, 0);
		}

		ReDisplay();
		return;
	}

	m_pParent->UpdateMenuState();
	ReDisplay();
}

/*!
 * アイドルコールバック関数
 * @param[in] x ユーザ定義変数
 */
void rxFlGLWindow::OnIdle_s(void* x)
{
	((rxFlGLWindow*)x)->Idle();
}
void rxFlGLWindow::Idle(void)
{
	make_current();

	if(m_iCurrentStep%RX_DISPLAY_STEP == 0){
		RXCOUT << "step " << m_iCurrentStep << endl;
	}
	// 出力用ステップ数
	int stp = m_iCurrentStep-2;
	stp = (m_iCurrentStep == 0) ? 0 : stp;
	stp = (stp < 0) ? 0 : stp;

	//
	// 前のフレームの描画を画像ファイルに保存
	//
	if(m_iSaveImageSpacing > 0){
		if(stp%m_iSaveImageSpacing == 0){
			SaveDisplay(stp);
		}
	}

	//
	// 前のフレームのデータをファイル保存
	//
	if(m_iSimuSetting & RX_SPH_OUTPUT){
		m_pPS->OutputParticles(CreateFileName(m_strSphOutputHeader, "dat", stp, 5));
	}

	g_TimerFPS.Reset();
	g_TimerFPS.Start();
	RXTIMER_RESET;


	//
	// シミュレーションタイムステップを進める
	//
	if(m_iSimuSetting & RX_SPH_INPUT){
		// パーティクル位置の入力
		if(!m_pPS->InputParticles(CreateFileName(m_strSphInputHeader, "dat", m_iCurrentStep, 5))){
			SwitchIdle(0);
			m_iSimuSetting &= ~RX_SPH_INPUT;
		}
	}
	else{
		RXTIMER_RESET;
		StepPS(m_fDt);
	}

	if(m_iPickedParticle != -1){
		SetParticleColorType(m_iColorType);
	}

	//
	// 流体表面メッシュ生成
	//
	if(m_iSimuSetting & RX_SPH_MESH){
		CalMeshSPH(m_iMeshMaxN, m_fMeshThr);
	}
	RXTIMER("mesh mc");

	//
	// FPSの計算
	//
	g_TimerFPS.Stop();
	if(m_iCurrentStep) ComputeFPS();

	if(m_iCurrentStep%RX_DISPLAY_STEP == 0){
		string tstr;
		RXTIMER_STRING(tstr);
		RXCOUT << "time : " << tstr << endl;
	}

	//
	// メッシュ保存
	//
	if(m_iSimuSetting & RX_SPH_MESH){
		if(m_iSimuSetting & RX_SPH_MESH_OUTPUT){// && m_iCurrentStep%m_iSaveMeshSpacing){
			SaveMesh(m_iCurrentStep, m_Poly);
		}
	}

	// 最大ステップ数を超えたらアイドルを止める
	if(m_iCurrentStep > RX_MAX_STEPS) SwitchIdle(0);

	m_iCurrentStep++;		// 現在のステップ数

	redraw();
}

/*!
 * タイマーコールバック関数
 * @param[in] x ユーザ定義変数
 */
void rxFlGLWindow::OnTimer_s(void* x)
{
	((rxFlGLWindow*)x)->Timer();
}
void rxFlGLWindow::Timer(void)
{
	Idle();
	if(m_bAnimation){
		Fl::repeat_timeout(0.033, OnTimer_s, this);
	}
}


/*!
 * アイドル関数のON/OFF
 * @param[in] on trueでON, falseでOFF
 */
bool rxFlGLWindow::SwitchIdle(int on)
{
	m_bAnimation = (on == -1) ? !m_bAnimation : (on ? true : false);
	if(m_bAnimation){
		//Fl::add_timeout(0.033, CB_Timer_s, this);
		if(!Fl::has_idle(rxFlGLWindow::OnIdle_s, this)){
			Fl::add_idle(rxFlGLWindow::OnIdle_s, this);
		}
	}
	else{
		if(Fl::has_idle(rxFlGLWindow::OnIdle_s, this)){
			Fl::remove_idle(rxFlGLWindow::OnIdle_s, this);
		}
	}
	return m_bAnimation;
}

/*!
 * フルスクリーン/ウィンドウ表示の切り替え
 * @param[in] win 1でGLキャンパスをウィンドウ内でフル化, 0でウィンドウ含めてフル化
 */
void rxFlGLWindow::SwitchFullScreen(int win)
{
	static int pos0[2] = { 0, 0 };
	static int win0[2] = { 500, 500 };
	if(win){
		// ウィンドウ内でフル化
		if(m_bFullscreen){
			fullscreen_off(pos0[0], pos0[1], win0[0], win0[1]);
			m_bFullscreen = false;
		}
		else if(m_pParent->IsFullScreen()){
			pos0[0] = x();
			pos0[1] = y();
			win0[0] = w();
			win0[1] = h();
			fullscreen();
			m_bFullscreen = true;
		}
	}
	else{
		// ウィンドウ含めてフル化
		if(m_bFullscreen){
			fullscreen_off(pos0[0], pos0[1], win0[0], win0[1]);
			m_bFullscreen = false;
			if(m_pParent->IsFullScreen()) m_pParent->SwitchFullScreen();
		}
		else{
			if(!m_pParent->IsFullScreen()) m_pParent->SwitchFullScreen();
			pos0[0] = x();
			pos0[1] = y();
			win0[0] = w();
			win0[1] = h();
			fullscreen();
			m_bFullscreen = true;
		}
	}
}

/*!
 * フルスクリーン/ウィンドウ表示の状態取得
 */
int rxFlGLWindow::IsFullScreen(void)
{
	return (m_bFullscreen ? 1 : 0);
}


/*!
 * ファイル読み込み
 * @param[in] fn ファイルパス
 */
void rxFlGLWindow::OpenFile(const string &fn)
{
	redraw();
}

/*!
 * ファイル書き込み
 * @param[in] fn ファイルパス
 */
void rxFlGLWindow::SaveFile(const string &fn)
{
}



/*!
 * 現在の画面描画を画像ファイルとして保存
 * @param[in] fn ファイルパス
 */
void rxFlGLWindow::SaveDisplay(const string &fn)
{
	int w_ = w();
	int h_ = h();
	int c_ = 4;

	//make_current();
	unsigned char* data = new unsigned char[w_*h_*c_];

	glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, data);

	// 上下反転
	int stride = w_*c_;
	for(int j = 0; j < h_/2; ++j){
		for(int i = 0; i < stride; ++i){
			unsigned char tmp = data[j*stride+i];
			data[j*stride+i] = data[(h_-j-1)*stride+i];
			data[(h_-j-1)*stride+i] = tmp;
		}
	}
	for(int j = 0; j < h_; ++j){
		for(int i = 0; i < w_; ++i){
			data[(j*w_+i)*c_+3] = 255;
		}
	}

	string fn1 = fn;
	string ext = GetExtension(fn);
	if(ext != "bmp"){
		fn1 += ".bmp";
	}

	WriteBitmapFile(fn1, data, w_, h_, c_, RX_BMP_WINDOWS_V3);
	cout << "saved the screen image to " << fn << endl;

	delete [] data;
}


/*!
 * 現在の画面描画を画像ファイルとして保存(連番)
 * @param[in] stp 現在のステップ数(ファイル名として使用)
 */
void rxFlGLWindow::SaveDisplay(const int &stp)
{
	string image_name = "sph";
	string head = RX_DEFAULT_IMAGE_DIR+image_name+"_";
	string fn = CreateFileName(head, ".png", stp, 5);

	SaveDisplay(fn);
}


/*!
 * メッシュをOBJファイルとして保存
 * @param[in] fn ファイル名
 * @param[in] polys ポリゴンオブジェクト
 */
void rxFlGLWindow::SaveMesh(const string fn, rxPolygons &polys)
{
	rxOBJ saver;
	rxMTL mtl;	// 材質はemptyのまま
	if(saver.Save(fn, polys.vertices, polys.normals, polys.faces, mtl)){
		RXCOUT << "saved the mesh to " << fn << endl;
	}
}

/*!
 * メッシュをOBJファイルとして保存(ステップ数からファイル名自動生成)
 * @param[in] stp 現在のステップ数(ファイル名として使用)
 * @param[in] polys ポリゴンオブジェクト
 */
void rxFlGLWindow::SaveMesh(const int &stp, rxPolygons &polys)
{
	SaveMesh(CreateFileName(RX_DEFAULT_MESH_DIR+"sph_", "obj", stp, 5), polys);
}


/*!
 * イベントハンドラ
 * @param[in] ev イベントID
 */
int rxFlGLWindow::handle(int e)
{
	switch(e){
	case FL_DND_ENTER:
	case FL_DND_RELEASE:
	case FL_DND_LEAVE:
	case FL_DND_DRAG:
	case FL_PASTE:
		// DnDBoxにペースト処理を渡すために，
		// これらのイベントが来たときはFl_Gl_Window::handleに処理を渡さない．
		return 1;

	default:
		break;
	}

	return Fl_Gl_Window::handle(e);
}


/*!
 * Fl_Menu_Barのコールバック関数 - Draw
 * @param[in] widget ウィジットの親クラスオブジェクト
 * @param[in] x ユーザ定義変数
 */
void rxFlGLWindow::OnMenuDraw_s(Fl_Widget *widget, void* x)
{
	Fl_Menu_Bar *menubar = (Fl_Menu_Bar*)widget;
	char picked[80];
	menubar->item_pathname(picked, sizeof(picked)-1);	// メニュー名

	string label = picked;
	string menu_name = "Draw/";
	label = label.substr(menu_name.size(), string::npos);

	((rxFlGLWindow*)x)->OnMenuDraw(1.0, label);
}
void rxFlGLWindow::OnMenuDraw(double val, string label)
{
	int idx = 0;
	while(label.find(RX_DRAW_STR[2*idx]) != 0) idx++;

	int flag = (0x01 << idx);
	m_iDraw ^= flag;

	switch(flag){
	case RXD_CELLS:	// 分割セル
		if(m_iDraw & RXD_CELLS){
			m_pPS->SetParticlesToCell();
			m_pPS->SetPolygonsToCell();
		}
		break;

	case RXD_MESH:	// メッシュ生成と描画
		SetMeshCreation();
		break;

	default:
		break;
	}
	
	m_pParent->UpdateMenuState();
	redraw();
}


/*!
 * Fl_Menu_Barのコールバック関数 - Simulation
 * @param[in] widget ウィジットの親クラスオブジェクト
 * @param[in] x ユーザ定義変数
 */
void rxFlGLWindow::OnMenuSimulation_s(Fl_Widget *widget, void* x)
{
	Fl_Menu_Bar *menubar = (Fl_Menu_Bar*)widget;
	char picked[80];
	menubar->item_pathname(picked, sizeof(picked)-1);	// メニュー名

	string label = picked;
	string menu_name = "Simulation/";
	label = label.substr(menu_name.size(), string::npos);

	((rxFlGLWindow*)x)->OnMenuSimulation(1.0, label);
}
void rxFlGLWindow::OnMenuSimulation(double val, string label)
{
	if(label.find("Reset") != string::npos){
		// シーンリセット
		if(m_pPS) delete m_pPS;
		m_pPS = 0;
		InitSPH(m_Scene);
	}
	else if(label.find("Artificial Pressure") != string::npos){
		// クラスタリングを防ぐための人工圧力のON/OFF (Tensile Instability)
		static_cast<RXSPH*>(m_pPS)->GetArtificialPressure() ^= 1;
	}
	else if(label.find("Particle Data Input") != string::npos){
		// パーティクルデータのインプット
		m_iSimuSetting ^= RX_SPH_INPUT;
		if(m_pPS) delete m_pPS;
		m_pPS = 0;
		InitSPH(m_Scene);
	}
	else if(label.find("Particle Data Output") != string::npos){
		// パーティクルデータのアウトプット
		m_iSimuSetting ^= RX_SPH_OUTPUT;
		if(m_iSimuSetting & RX_SPH_OUTPUT) m_pPS->OutputSetting(m_strSphOutputName0);
	}
	else if(label.find("Mesh Saving") != string::npos){
		// メッシュ保存	
		m_iSimuSetting ^= RX_SPH_MESH_OUTPUT;
	}
	else if(label.find("Image Saving") != string::npos){
		// 画面の定期画像保存
		if(m_iSaveImageSpacing != -1){
			m_iSaveImageSpacing = -1;
		}
		else{
			m_iSaveImageSpacing = RX_SAVE_IMAGE_SPACING;
			RXCOUT << "save frames per " << m_iSaveImageSpacing << " steps" << endl;
		}
	}
	
	m_pParent->UpdateMenuState();
	redraw();
}


/*!
 * Fl_Menu_Barのコールバック関数 - Particle
 * @param[in] widget ウィジットの親クラスオブジェクト
 * @param[in] x ユーザ定義変数
 */
void rxFlGLWindow::OnMenuParticle_s(Fl_Widget *widget, void* x)
{
	Fl_Menu_Bar *menubar = (Fl_Menu_Bar*)widget;
	char picked[80];
	menubar->item_pathname(picked, sizeof(picked)-1);	// メニュー名

	string label = picked;
	string menu_name = "Particle/";
	label = label.substr(menu_name.size(), string::npos);

	if(label.find("Color/") != string::npos){
		menu_name = "Color/";
		label = label.substr(menu_name.size(), string::npos);
		((rxFlGLWindow*)x)->OnMenuParticleColor(1.0, label);
	}
	else if(label.find("Draw/") != string::npos){
		menu_name = "Draw/";
		label = label.substr(menu_name.size(), string::npos);
		((rxFlGLWindow*)x)->OnMenuParticleDraw(1.0, label);
	}
	else{
		((rxFlGLWindow*)x)->OnMenuParticle(1.0, label);
	}
}
/*!
 * Particleメニューのイベントハンドラ
 */
void rxFlGLWindow::OnMenuParticle(double val, string label)
{
	m_pParent->UpdateMenuState();
	redraw();
}
/*!
 * Particle/Colorメニューのイベントハンドラ
 */
void rxFlGLWindow::OnMenuParticleColor(double val, string label)
{
	int idx = 0;
	while(label.find(RX_PARTICLE_COLOR[2*idx]) == string::npos) idx++;
	SetParticleColorType(idx);
	m_pParent->UpdateMenuState();
	redraw();
}

/*!
 * Particle/Drawメニューのイベントハンドラ
 */
void rxFlGLWindow::OnMenuParticleDraw(double val, string label)
{
	int idx = 0;
	while(label.find(RX_PARTICLE_DRAW[2*idx]) == string::npos) idx++;
	m_iDrawPS = idx;
	m_pParent->UpdateMenuState();
	redraw();
}


/*!
 * Fl_Menu_Barのコールバック関数 - Solid
 * @param[in] widget ウィジットの親クラスオブジェクト
 * @param[in] x ユーザ定義変数
 */
void rxFlGLWindow::OnMenuMesh_s(Fl_Widget *widget, void* x)
{
	Fl_Menu_Bar *menubar = (Fl_Menu_Bar*)widget;
	char picked[80];
	menubar->item_pathname(picked, sizeof(picked)-1);	// メニュー名

	string label = picked;
	string menu_name = "Mesh/";
	label = label.substr(menu_name.size(), string::npos);

	if(label.find("Solid/") != string::npos){
		menu_name = "Solid/";
		label = label.substr(menu_name.size(), string::npos);
		((rxFlGLWindow*)x)->OnMenuMeshSolid(1.0, label);
	}
	else if(label.find("Liquid/") != string::npos){
		menu_name = "Liquid/";
		label = label.substr(menu_name.size(), string::npos);
		((rxFlGLWindow*)x)->OnMenuMeshLiquid(1.0, label);
	}
	else{
		((rxFlGLWindow*)x)->OnMenuMesh(1.0, label);
	}
}
void rxFlGLWindow::OnMenuMesh(double val, string label)
{
}
void rxFlGLWindow::OnMenuMeshSolid(double val, string label)
{
	int idx = 0;
	while(label.find(RXS_STR[2*idx]) == string::npos) idx++;
	int flag = (0x01 << idx);
	m_iSolidDraw ^= flag;
	m_pParent->UpdateMenuState();
	redraw();
}
void rxFlGLWindow::OnMenuMeshLiquid(double val, string label)
{
	int idx = 0;
	while(label.find(RXL_STR[2*idx]) == string::npos) idx++;
	int flag = (0x01 << idx);
	m_iLiquidDraw ^= flag;
	m_pParent->UpdateMenuState();
	redraw();
}




/*!
 * Fl_Menu_Barのコールバック関数 - Scene
 * @param[in] widget ウィジットの親クラスオブジェクト
 * @param[in] x ユーザ定義変数
 */
void rxFlGLWindow::OnMenuScene_s(Fl_Widget *widget, void* x)
{
	Fl_Menu_Bar *menubar = (Fl_Menu_Bar*)widget;
	char picked[80];
	menubar->item_pathname(picked, sizeof(picked)-1);	// メニュー名

	string label = picked;
	string menu_name = "Scene/";
	label = label.substr(menu_name.size(), string::npos);

	((rxFlGLWindow*)x)->OnMenuScene(1.0, label);
}
void rxFlGLWindow::OnMenuScene(double val, string label)
{
	if(m_Scene.SetCurrentSceneFromTitle(label)){
		m_iCurrentSceneIdx = m_Scene.GetCurrentSceneIdx();
		InitSPH(m_Scene);
	}
	
	m_pParent->UpdateMenuState();
	redraw();
}



//-----------------------------------------------------------------------------
// MARK:メッシュ作成
//-----------------------------------------------------------------------------
/*!
 * 三角形メッシュ生成
 * @param[in] nmax メッシュ化グリッド解像度(最大)
 * @param[in] thr メッシュ化閾値
 */
bool rxFlGLWindow::CalMeshSPH(int nmax, double thr)
{
#if defined(RX_USE_GPU)
	return calMeshSPH_GPU(nmax, thr);
#else
	return calMeshSPH_CPU(nmax, thr);
#endif
}

/*!
 * メッシュ情報の初期化
 */
bool rxFlGLWindow::ResetMesh(void)
{
	// ポリゴン初期化
	if(!m_Poly.vertices.empty()){
		m_Poly.vertices.clear();
		m_Poly.normals.clear();
		m_Poly.faces.clear();
		m_Poly.materials.clear();
	}
	m_iNumVrts = 0;
	m_iNumTris = 0;

	// 頂点VBO
	if(!m_uVrtVBO) glDeleteBuffers(1, &m_uVrtVBO);
	m_uVrtVBO = 0;

	// 法線VBO
	if(!m_uNrmVBO) glDeleteBuffers(1, &m_uNrmVBO);
	m_uNrmVBO = 0;

	// メッシュVBO
	if(m_uTriVBO) glDeleteBuffers(1, &m_uTriVBO);
	m_uTriVBO = 0;

	if(m_pMCMeshCPU) delete m_pMCMeshCPU;
	if(m_pMCMeshGPU) delete m_pMCMeshGPU;


	m_pMCMeshCPU = 0;
	m_pMCMeshGPU = 0;

	return true;
}


/*!
 * メッシュ生成のセット
 */
void rxFlGLWindow::SetMeshCreation(void)
{
	if(m_iDraw & RXD_MESH){
		m_iSimuSetting |= RX_SPH_MESH;
	}
	else{
		m_iSimuSetting &= ~RX_SPH_MESH;
	}
	if((m_iSimuSetting & RX_SPH_MESH) && m_iCurrentStep){
		CalMeshSPH(m_iMeshMaxN, m_fMeshThr);
	}
}

/*!
 * 三角形メッシュの生成(MC法,CPU)
 * @param[in] nmax メッシュ化グリッド解像度(最大)
 * @param[in] thr メッシュ化閾値
 */
bool rxFlGLWindow::calMeshSPH_CPU(int nmax, double thr)
{
	// パーティクルを近傍探索用均一グリッドに格納
	m_pPS->SetParticlesToCell();

	Vec3 minp = m_pPS->GetMin();
	Vec3 maxp = m_pPS->GetMax();

	// 境界矩形の最大幅の軸方向を基準に分割数を設定(セルが立方体になるようにする)
	double h;
	int n[3];
	CalMeshDiv(minp, maxp, nmax, h, n, 0.05);
	for(int i = 0; i < 3; ++i) m_iMeshN[i] = n[i];

	// ポリゴン初期化
	if(!m_Poly.vertices.empty()){
		m_Poly.vertices.clear();
		m_Poly.normals.clear();
		m_Poly.faces.clear();
		m_Poly.materials.clear();
	}

	if(!m_pMCMeshCPU){
		m_pMCMeshCPU = new rxMCMeshCPU;
	}

	// メッシュ生成
	m_pMCMeshCPU->CreateMesh(&RXSPH::GetImplicit_s, m_pPS, minp, h, n, thr, m_Poly.vertices, m_Poly.normals, m_Poly.faces);

	m_iNumVrts = (int)m_Poly.vertices.size();
	m_iNumTris = (int)m_Poly.faces.size();
	
	if(m_Poly.normals.empty()){
		CalVertexNormals(m_Poly);
	}
	m_Poly.materials[m_matPoly.name] = m_matPoly;

	glDeleteBuffers(1, &m_uVrtVBO); m_uVrtVBO = 0;
	glDeleteBuffers(1, &m_uNrmVBO); m_uNrmVBO = 0;
	glDeleteBuffers(1, &m_uTriVBO); m_uTriVBO = 0;

	return true;
}

/*!
 * 三角形メッシュの生成(MC法,GPU)
 * @param[in] nmax メッシュ化グリッド解像度(最大)
 * @param[in] thr メッシュ化閾値
 */
bool rxFlGLWindow::calMeshSPH_GPU(int nmax, double thr)
{
	if(m_pPS == NULL) return false;

	Vec3 minp = m_vMeshBoundaryCen-m_vMeshBoundaryExt;
	Vec3 maxp = m_vMeshBoundaryCen+m_vMeshBoundaryExt;

	double h;
	int n[3];
	CalMeshDiv(minp, maxp, nmax, h, n, 0.05);
	for(int i = 0; i < 3; ++i) m_iMeshN[i] = n[i];
	//RXCOUT << "mc : " << minp << " - " << maxp << endl;
	//RXCOUT << "mc : " << n[0] << " x " << n[1] << " x " << n[2] << endl;

	m_iDimVBO = 4;

	if(!m_pMCMeshGPU){
		m_pMCMeshGPU = new rxMCMeshGPU;
		m_pMCMeshGPU->Set(minp, Vec3(h), n, m_iVertexStore);

		// メッシュデータ格納用のVBOの確保
		AssignArrayBuffers(m_pMCMeshGPU->GetMaxVrts(), 4, m_uVrtVBO, m_uNrmVBO, m_uTriVBO);
	}

	if(!m_Poly.vertices.empty()){
		m_Poly.vertices.clear();
		m_Poly.normals.clear();
		m_Poly.faces.clear();
		m_Poly.materials.clear();
	}


	// サンプリングボリュームの計算(陰関数値を格納したグリッドデータ)
	m_pPS->CalImplicitFieldDevice(n, minp, Vec3(h, h, h), m_pMCMeshGPU->GetSampleVolumeDevice());

	unsigned int nvrts = 0;
	unsigned int ntris = 0;
	m_pMCMeshGPU->CreateMeshV(minp, h, n, thr, nvrts, ntris);

	m_iNumVrts = nvrts;
	m_iNumTris = ntris;
	if(m_iCurrentStep%RX_DISPLAY_STEP == 0){
		RXCOUT << nvrts << " verts and " << ntris << " tri were created." << endl;
	}

	// FBOにデータをコピー
	m_pMCMeshGPU->SetDataToFBO(m_uVrtVBO, m_uNrmVBO, m_uTriVBO);

	// ファイル保存のためにメッシュデータをホスト側配列にコピー
	if(m_iSimuSetting & RX_SPH_MESH_OUTPUT){
		m_pMCMeshGPU->SetDataToArray(m_Poly.vertices, m_Poly.normals, m_Poly.faces);
		CalVertexNormals(m_Poly);
	}

	return true;
}



//-----------------------------------------------------------------------------
// SPH関数
//-----------------------------------------------------------------------------
/*!
 * SPHの初期化
 * @param[in] fn_scene シーン記述ファイル名
 */
void rxFlGLWindow::InitSPH(rxSceneConfig &sph_scene)
{
	// SPHクラスの初期化とシーンクラスへの設定
	if(m_pPS) delete m_pPS;
	m_pPS = new RXSPH(true);
	sph_scene.Clear();
	sph_scene.Set(m_pPS);
		
	// シーン全体情報の読み込み
	if((m_iSimuSetting & RX_SPH_INPUT) && ExistFile(m_strSphInputName0)){
		sph_scene.LoadSpaceFromFile(m_strSphInputName0);
	}
	else{
		if(sph_scene.LoadSpaceFromFile()){
			rxEnviroment sph_env = sph_scene.GetEnv();
			((RXSPH*)m_pPS)->Initialize(sph_env);

			m_fDt = sph_env.dt;
			m_iVertexStore = sph_env.mesh_vertex_store;
			m_iMeshMaxN = sph_env.mesh_max_n;

			if(sph_env.use_inlet){
				m_iSimuSetting |= RX_SPH_INLET;
			}
			else{
				m_iSimuSetting &= ~RX_SPH_INLET;
			}
		}
	}
	m_iCurrentStep = 0;

	// パーティクル描画のためのカラータイプの設定
	SetParticleColorType(m_iColorType);

	// シーンの個別設定(パーティクル，固体)の読み込み
	if(m_iSimuSetting & RX_SPH_INPUT){
		// パーティクル情報をファイルから読み込み
		m_pPS->InputParticles(CreateFileName(m_strSphInputHeader, "dat", 0, 5));
		CalMeshSPH(m_iMeshMaxN, m_fMeshThr);
	}
	else{
		m_pPS->Reset(rxParticleSystemBase::RX_CONFIG_NONE);
		sph_scene.LoadSceneFromFile();
	}

	m_pPS->InitBoundary();


	// メッシュ生成境界の設定
	m_vMeshBoundaryCen = sph_scene.GetEnv().mesh_boundary_cen;
	m_vMeshBoundaryExt = sph_scene.GetEnv().mesh_boundary_ext;
	cout << "GLWindow : mesh boundary : " << m_vMeshBoundaryCen << " - " << m_vMeshBoundaryExt << endl;

	// 表面メッシュのリセット
	ResetMesh();

	if(m_iSimuSetting & RX_SPH_OUTPUT) m_pPS->OutputSetting(m_strSphOutputName0);

	// 時間計測用変数の初期化
	g_fTotalTime = 0.0f;
	g_fAvgTime = 0.0f;
	g_iTimeCount = 0;
}


/*!
 * シーンに水滴を追加
 */
void rxFlGLWindow::AddSphere(void)
{
	int ballr = 10;

	RXREAL pr = m_pPS->GetParticleRadius();
	RXREAL tr = pr+(pr*2.0f)*ballr;
	RXREAL pos[4], vel[4];
	pos[0] = -1.0+tr+RX_FRAND()*(2.0-tr*2.0);
	pos[1] =  1.0-tr;
	pos[2] = -1.0+tr+RX_FRAND()*(2.0-tr*2.0);
	pos[3] =  0.0;
	vel[0] = vel[1] = vel[2] = vel[3] = 0.0;
	m_pPS->AddSphere(0, pos, vel, ballr, pr*2.0f);
}


/*!
 * SPHのタイムステップを進める
 * @param[in] dt タイムステップ幅
 */
void rxFlGLWindow::StepPS(double dt)
{
	if(!m_bPause){
		float damping = 1.0f;

		// シミュレーションパラメータ
		m_pPS->SetGravity((RXREAL)(-m_fGravity));

		// シミュレーションステップを進める
		m_pPS->Update((RXREAL)dt, m_iCurrentStep); 
	}
}

/*!
 * FPSを計算してウィンドウタイトルに表示
 */
void rxFlGLWindow::ComputeFPS(void)
{
	static unsigned int frame_count = 0;
	static bool verify = false;
	static int fps_count = 0;
	static int fps_limit = 1;

	frame_count++;
	fps_count++;
	if(fps_count == fps_limit-1){
		verify = true;
	}
	if(fps_count == fps_limit){
		char fps[256];
		
		g_fTotalTime += g_TimerFPS.GetTime(0);
		g_iTimeCount++;
		g_fAvgTime = g_fTotalTime/(float)g_iTimeCount;

		int num_particles = m_pPS->GetNumParticles();
		double ifps = 1.0/g_fAvgTime;
		sprintf(fps, "%d particles : %d step, %3.1f fps", num_particles, m_iCurrentStep, ifps);  

		m_pParent->SetStatusLabel(fps);
		fps_count = 0; 
		
		g_TimerFPS.Reset();
	}
}

/*!
 * "\n"が含まれるstringを複数のstringに分解する
 * @param[in] org 元の文字列
 * @param[in] div 分解後の文字列配列
 */
void rxFlGLWindow::DivideString(const string &org, vector<string> &div)
{
	size_t pos0 = 0, pos1 = 0;
	while(pos1 != string::npos){
		pos1 = org.find("\n", pos0);

		div.push_back(org.substr(pos0, pos1-pos0));

		pos0 = pos1+1;
	}
}

/*!
 * 画面出力用の文字列の生成
 * @return 文字列
 */
vector<string> rxFlGLWindow::SetDrawString(void)
{
	vector<string> str;

	str.push_back("Color : ");
	int type = m_pPS->GetColorType();
	if(type == rxParticleSystemBase::RX_DENSITY) str.back() << STR(RX_DENSITY);
	else if(type == rxParticleSystemBase::RX_RAMP) str.back() << STR(RX_RAMP);
	else str.back() << STR(RX_NONE);

	str.back() << ", Drawing : ";
	if((m_iDraw & RXD_REFRAC)) str.back() << "refraction mesh";
	else if(m_iDrawPS == RXP_POINT) str.back() << STR(RXP_POINT);
	else if(m_iDrawPS == RXP_POINTSPRITE) str.back() << STR(RXP_POINTSPRITE);
	else if(m_iDrawPS == RXP_POINT_NONE) str.back() << STR(RXP_POINT_NONE);

	str.push_back("");
#if defined(RX_USE_GPU)
	str.back() << "PBDSPH(GPU)";
#else
	str.back() << "PBDSPH";
#endif

	if((m_iSaveImageSpacing > 0) || (m_iSimuSetting & RX_SPH_OUTPUT) || (m_iSimuSetting & RX_SPH_MESH_OUTPUT)){
		str.push_back("");
		str.back() << "Output : " << (m_iSaveImageSpacing > 0 ? "img " : "");
		str.back() << ((m_iSimuSetting & RX_SPH_OUTPUT) ? "prts " : "");
		str.back() << ((m_iSimuSetting & RX_SPH_MESH_OUTPUT) ? "mesh " : "");
		if(m_iSimuSetting & RX_SPH_INPUT){
			str.back() << ", Input : " << ((m_iSimuSetting & RX_SPH_INPUT) ? "prts " : "");
		}
	}

	str.push_back("num particles : ");
	str.back() << m_pPS->GetNumParticles();
	if(m_pPS->GetNumBoundaryParticles()){
		str.back() << " (boundary : " << m_pPS->GetNumBoundaryParticles() << ")";
	}

	// 反復
	str.push_back("");
	str.back() << "Iterations : " << g_iIterations;
	str.push_back("");
	str.back() << "Eta : " << g_fEta;
	str.push_back("Artificial pressure : ");
	str.back() << (static_cast<RXSPH*>(m_pPS)->GetArtificialPressure() ? "on" : "off");

	// メッシュ
	if(m_iSimuSetting & RX_SPH_MESH){
		str.push_back("MC : ");
		str.back() << "grid (" << m_iMeshN[0] << "x" << m_iMeshN[1] << "x" << m_iMeshN[2] << "), ";
		str.back() << "vertex " << m_iNumVrts << ", face " << m_iNumTris;
	}

	// 計測された計算時間
	str.push_back("time : ");
	string tstr;
	RXTIMER_STRING(tstr);
	DivideString(tstr, str);

	return str;
}


/*!
 * パーティクル速度をGL_LINESで描画
 * @param[in] prts パーティクル位置
 * @param[in] vels パーティクル速度
 * @param[in] n パーティクル数
 * @param[in] d 配列のステップ
 * @param[in] len 線の長さ
 */
void rxFlGLWindow::DrawParticleVector(RXREAL *prts, RXREAL *vels, int n, int d, double *c0, double *c1, double len)
{
	glBegin(GL_LINES);
	int k = 0;
	for(int i = 0; i < n; ++i){
		glColor3dv(c0);
		glVertex3d(prts[k], prts[k+1], prts[k+2]);
		glColor3dv(c1);
		glVertex3d(prts[k]+len*vels[k], prts[k+1]+len*vels[k+1], prts[k+2]+len*vels[k+2]);
		k += d;
	}
	glEnd();
}

/*!
 * パーティクルをGL_POINTSで描画
 *  - VBOがあれば用いる
 * @param[in] vbo パーティクル位置を格納したVBO
 * @param[in] n パーティクル数
 * @param[in] color_vbo パーティクルの色を格納したVBO
 * @param[in] data パーティクル座標(ホスト側メモリ，vboが0の時に使用)
 * @param[in] offset 描画するパーティクルの最初のインデックス
 */
void rxFlGLWindow::DrawParticlePoints(unsigned int vbo, int n, unsigned int color_vbo, RXREAL *data, int offset)
{
	if(!vbo){
		glBegin(GL_POINTS);
		int k = offset*4;
		for(int i = offset; i < n; ++i){
			glVertex3d(data[k], data[k+1], data[k+2]);
			k += 4;
		}
		glEnd();
	}
	else{
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glEnableClientState(GL_VERTEX_ARRAY);
		glVertexPointer(4, GL_REAL, 0, 0);

		if(color_vbo){
			glBindBufferARB(GL_ARRAY_BUFFER_ARB, color_vbo);
			glColorPointer(4, GL_REAL, 0, 0);
			glEnableClientState(GL_COLOR_ARRAY);
		}

		glDrawArrays(GL_POINTS, offset, n-offset);

		glDisableClientState(GL_VERTEX_ARRAY); 
		glDisableClientState(GL_COLOR_ARRAY); 
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
}



/*!
 * Vertex Buffer Object(VBO)の作成
 * @param[inout] vbo バッファID
 * @param[in] size バッファサイズ
 */
void rxFlGLWindow::CreateVBO(GLuint* vbo, unsigned int size)
{
	glGenBuffers(1, vbo);
	glBindBuffer(GL_ARRAY_BUFFER, *vbo);

	// バッファオブジェクトの初期化
	glBufferData(GL_ARRAY_BUFFER, size, 0, GL_DYNAMIC_COPY);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/*!
 * Vertex Buffer Object(VBO)の削除
 * @param[inout] vbo バッファID
 */
void rxFlGLWindow::DeleteVBO(GLuint* vbo)
{
	glBindBuffer(1, *vbo);
	glDeleteBuffers(1, vbo);

	*vbo = 0;
}


/*!
 * VBOを用いた等値面ポリゴン描画
 */
void rxFlGLWindow::DrawLiquidSurface(void)
{
	if(m_uVrtVBO){
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_NORMAL_ARRAY);

		glBindBuffer(GL_ARRAY_BUFFER, m_uVrtVBO);
		glVertexPointer(4, GL_FLOAT, 0, 0);
		glBindBuffer(GL_ARRAY_BUFFER, m_uNrmVBO);
		glNormalPointer(GL_FLOAT, 0, 0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_uTriVBO);

		if(m_iLiquidDraw & RXL_FACE){
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			glEnable(GL_LIGHTING);
			glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
			glEnable(GL_COLOR_MATERIAL);
			glColor4d(0.0, 0.2, 1.0, 1.0);
			glDrawElements(GL_TRIANGLES, m_iNumTris*3, GL_UNSIGNED_INT, 0);
		}
		if(m_iLiquidDraw & RXL_EDGE){
			glEnable(GL_POLYGON_OFFSET_LINE);
			glPolygonOffset(1.0, 1.0);
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			glDisable(GL_LIGHTING);
			glColor3d(0.3, 1.0, 0.0);
			glLineWidth(2.0);
			glDrawElements(GL_TRIANGLES, m_iNumTris*3, GL_UNSIGNED_INT, 0);
		}
		if(m_iLiquidDraw & RXL_VERTEX){
			glDisable(GL_LIGHTING);
			glColor3d(1.0, 0.0, 0.0);
			glPointSize(5.0);
			glDrawElements(GL_POINTS, m_iNumTris*3, GL_UNSIGNED_INT, 0);
		}
		 
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

		glDisableClientState(GL_VERTEX_ARRAY); 
		glDisableClientState(GL_NORMAL_ARRAY); 
	}
	else{
		glEnable(GL_LIGHTING);
		glColor4d(0.0, 1.0, 0.0, 1.0);
		m_Poly.Draw(m_iLiquidDraw);
	}
}

/*!
 * パーティクル描画時の色の設定
 * @param[in] type 色わけの種類
 */
void rxFlGLWindow::SetParticleColorType(int type)
{
	int ps_type = rxParticleSystemBase::RX_CONSTANT;
	switch(type){
	default:
	case RXP_COLOR_CONSTANT: ps_type = rxParticleSystemBase::RX_CONSTANT; break;
	case RXP_COLOR_DENSITY:  ps_type = rxParticleSystemBase::RX_DENSITY; break;
	case RXP_COLOR_RAMP:	 ps_type = rxParticleSystemBase::RX_RAMP; break;
	}
	m_pPS->SetColorType(ps_type);
	m_pPS->SetColorVBO(m_iPickedParticle);
	m_iColorType = type;
}

/*!
 * シーン描画
 */
void rxFlGLWindow::RenderSphScene(void)
{
	glEnable(GL_LIGHTING);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDepthFunc(GL_LESS);

	int pnum = m_pPS->GetNumParticles();
	Vec3 cen = m_pPS->GetCen();
	Vec3 dim = m_pPS->GetDim();

	// 周囲境界
	if((m_iDraw & RXD_BBOX)){
		DrawWireCuboid(cen-0.5*dim, cen+0.5*dim, Vec3(0.0));
	}

	// 軸
	if((m_iDraw & RXD_AXIS)){
		DrawAxis(0.6*dim[0], 3.0);
	}


	// 障害物(固体境界)
	if((m_iDraw & RXD_SOLID)){
		glDisable(GL_LIGHTING);
		glColor4d(0.0, 1.0, 0.0, 1.0);
		glLineWidth(1.0);
		m_pPS->DrawObstacles(m_iSolidDraw);
	}
	
	// パーティクル
	if((m_iDraw & RXD_PARTICLE) && !(m_iDraw & RXD_REFRAC)){
		glDisable(GL_LIGHTING);
		glColor4d(0.0, 0.0, 1.0, 1.0);
		glEnable(GL_POINT_SMOOTH);
		glPointSize(1.0);

		unsigned int pvbo = m_pPS->GetCurrentReadBuffer();
		unsigned int cvbo = m_pPS->GetColorBuffer();

		if(m_iDrawPS == RXP_POINT){				// GL_POINTSで描画
			glPointSize(8.0);
			RXREAL *data = 0;
			data = m_pPS->GetArrayVBO(rxParticleSystemBase::RX_POSITION);
			DrawParticlePoints(0, pnum, cvbo, data);
		}
		else if(m_iDrawPS == RXP_POINTSPRITE){	// ポイントスプライトで描画
			float prad = (float)m_pPS->GetParticleRadius()*g_fPScale;			// パーティクル半径
			float pscale = 1.2*m_iWinH/tanf(RX_FOV*0.5f*(float)RX_PI/180.0f);

			glEnable(GL_POINT_SPRITE_ARB);
			glTexEnvi(GL_POINT_SPRITE_ARB, GL_COORD_REPLACE_ARB, GL_TRUE);
			glEnable(GL_VERTEX_PROGRAM_POINT_SIZE_NV);
			glDepthMask(GL_TRUE);
			glEnable(GL_DEPTH_TEST);

			glUseProgram(g_glslPointSprite.Prog);
			glUniform1f( glGetUniformLocation(g_glslPointSprite.Prog, "pointScale"), pscale );
			glUniform1f( glGetUniformLocation(g_glslPointSprite.Prog, "pointRadius"), prad );
			glUniform3f( glGetUniformLocation(g_glslPointSprite.Prog, "lightDir"), 0.6, 0.6, 0.6);
			if(m_iDraw & RXD_ZPLANE_CLIP){
				glUniform1f( glGetUniformLocation(g_glslPointSprite.Prog, "zCrossFront"),  m_fClipPlane[0]*0.5*dim[2]);
				glUniform1f( glGetUniformLocation(g_glslPointSprite.Prog, "zCrossBack"), m_fClipPlane[1]*0.5*dim[2]);
			}
			else{
				glUniform1f( glGetUniformLocation(g_glslPointSprite.Prog, "zCrossFront"), 1000.0);
				glUniform1f( glGetUniformLocation(g_glslPointSprite.Prog, "zCrossBack"), -1000.0);
			}

			DrawParticlePoints(pvbo, pnum, cvbo, 0);

			glUseProgram(0);
			glDisable(GL_POINT_SPRITE_ARB);
		}
		else if(m_iDrawPS == RXP_SPHERE){	// 球として描画
			glEnable(GL_LIGHTING);
			float prad = (float)m_pPS->GetParticleRadius()*g_fPScale;			// パーティクル半径
			RXREAL *data = 0;
			data = m_pPS->GetArrayVBO(rxParticleSystemBase::RX_POSITION);
			glColor3f(0.4, 0.4, 1.0);
			int k = 0;
			for(int i = 0; i < pnum; ++i){
				if((m_iDraw & RXD_ZPLANE_CLIP) && (data[k+2] > m_fClipPlane[0] || data[k+2] < m_fClipPlane[1])){
					k += 4;
					continue;
				}

				glPushMatrix();
				glTranslatef(data[k], data[k+1], data[k+2]);
				glRotated(90, 1.0, 0.0, 0.0);
				glutSolidSphere(prad, 20, 10);
				glPopMatrix();
				k += 4;
			}
		}

		if(m_iPickedParticle != -1){
			RXREAL prad = m_pPS->GetParticleRadius();
			int k = m_iPickedParticle;
			RXREAL *data = m_pPS->GetArrayVBO(rxParticleSystemBase::RX_POSITION);
			Vec3 pos(data[DIM*k+0], data[DIM*k+1], data[DIM*k+2]);
			glDisable(GL_LIGHTING);
			glColor3d(1.0, 1.0, 0.0);
			glPushMatrix();
			glTranslatef(pos[0], pos[1], pos[2]);
			glutSolidSphere(1.5*prad, 8, 4);
			glPopMatrix();


		}
	}

	//
	// 境界パーティクル
	//
	if(m_iDraw & RXD_BPARTICLE){
		glDisable(GL_LIGHTING);
		glColor4d(0.0, 1.0, 0.0, 1.0);
		glEnable(GL_POINT_SMOOTH);
		glPointSize(3.0);

		int bnum = m_pPS->GetNumBoundaryParticles();
		RXREAL *data = m_pPS->GetArrayVBO(rxParticleSystemBase::RX_BOUNDARY_PARTICLE, true, bnum);
		int boffset = m_pPS->m_uNumBParticles0;
		if(m_iDraw & RXD_BPARTICLE_ENV){
			boffset = 0;
		}
		DrawParticlePoints(0, bnum, 0, data, boffset);
	}


	//
	// メッシュ描画
	//
	if((m_iDraw & RXD_MESH)){
		if((m_iDraw & RXD_REFRAC) && m_bUseCubeMap){
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			glEnable(GL_LIGHTING);

			Vec3 eye_pos(0.0);		// 視点位置
			m_tbView.CalLocalPos(eye_pos.data, Vec3(0.0).data);
			//m_tbView.CalLocalRot(eye_dir.data, Vec3(0.0, 0.0, -1.0).data);
			
			GLuint prog = g_glslFresnel.Prog;
			glUseProgram(g_glslFresnel.Prog);

			// パラメータ設定
			// バーテックスシェーダ用パラメータ
			glUniform1f(glGetUniformLocation(g_glslFresnel.Prog, "etaRatio"), 0.78);
			glUniform1f(glGetUniformLocation(g_glslFresnel.Prog, "fresnelBias"), 0.27);
			glUniform1f(glGetUniformLocation(g_glslFresnel.Prog, "fresnelPower"), 0.98);
			glUniform1f(glGetUniformLocation(g_glslFresnel.Prog, "fresnelScale"), 5.0);
			glUniform3f(glGetUniformLocation(g_glslFresnel.Prog, "eyePosition"), eye_pos[0], eye_pos[1], eye_pos[2]);

			// フラグメントシェーダ用パラメータ
			glUniform1i(glGetUniformLocation(g_glslFresnel.Prog, "envmap"), 0);

			DrawLiquidSurface();

			glUseProgram(0);
		}
		else{
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			glDisable(GL_LIGHTING);
			glEnable(GL_CULL_FACE);
			glColor4d(0.3, 0.3, 1.0, 1.0);
			DrawLiquidSurface();
		}
	}

	//
	// パーティクル速度
	//
	if((m_iDraw & RXD_VELOCITY)){
		RXREAL *p = m_pPS->GetArrayVBO(rxParticleSystemBase::RX_POSITION);
		RXREAL *v = m_pPS->GetArrayVBO(rxParticleSystemBase::RX_VELOCITY);
		if(p && v){
			glDisable(GL_LIGHTING);
			glLineWidth(1.0);
			glColor3d(0.0, 1.0, 1.0);
			DrawParticleVector(p, v, pnum, DIM, Vec3(0.8, 0.8, 1.0).data, Vec3(0.0, 0.0, 1.0).data, norm(dim)*m_fVScale);
		}
	}


	// 
	// 分割セル
	//
	if((m_iDraw & RXD_CELLS)){
		glDisable(GL_LIGHTING);
		glLineWidth(1.0);
		m_pPS->DrawCells(Vec3(0.0, 1.0, 0.0), Vec3(1.0, 0.0, 0.0));
	}
}

/*!
 * 人口圧力項のON/OFF取得(メニュー更新用)
 */
bool rxFlGLWindow::IsArtificialPressureOn(void)
{
	if(m_pPS){
		return static_cast<RXSPH*>(m_pPS)->GetArtificialPressure();
	}
	return false;
}



//-----------------------------------------------------------------------------
// OpenGL描画関数
//-----------------------------------------------------------------------------
/*!
 * xyz軸描画(x軸:赤,y軸:緑,z軸:青)
 * @param[in] len 軸の長さ
 */
int DrawAxis(double len, double line_width)
{
	glLineWidth((GLfloat)line_width);

	// x軸
	glColor3f(1.0, 0.0, 0.0);
	glBegin(GL_LINES);
	glVertex3d(0.0, 0.0, 0.0);
	glVertex3d(len, 0.0, 0.0);
	glEnd();

	// y軸
	glColor3f(0.0, 1.0, 0.0);
	glBegin(GL_LINES);
	glVertex3d(0.0, 0.0, 0.0);
	glVertex3d(0.0, len, 0.0);
	glEnd();

	// z軸
	glColor3f(0.0, 0.0, 1.0);
	glBegin(GL_LINES);
	glVertex3d(0.0, 0.0, 0.0);
	glVertex3d(0.0, 0.0, len);
	glEnd();

	return 1;
}


/*! 
 * 直方体のワイヤーフレーム描画
 * @param[in] min 最小座標値
 * @param[in] max 最大座標値
 * @param[in] color 描画色
 */
void DrawWireCuboid(const Vec3 &min, const Vec3 &max, const Vec3 &color)
{
	glPushMatrix();
	glColor3f(color[0], color[1], color[2]);

	glBegin(GL_LINES);
	
	// x軸平行
	glVertex3f(min[0], min[1], min[2]);	glVertex3f(max[0], min[1], min[2]);
	glVertex3f(min[0], min[1], max[2]); glVertex3f(max[0], min[1], max[2]);
	glVertex3f(min[0], max[1], min[2]);	glVertex3f(max[0], max[1], min[2]);
	glVertex3f(min[0], max[1], max[2]);	glVertex3f(max[0], max[1], max[2]);
	
	// z軸平行
	glVertex3f(min[0], min[1], min[2]);	glVertex3f(min[0], min[1], max[2]);
	glVertex3f(min[0], max[1], min[2]);	glVertex3f(min[0], max[1], max[2]);
	glVertex3f(max[0], min[1], min[2]);	glVertex3f(max[0], min[1], max[2]);
	glVertex3f(max[0], max[1], min[2]);	glVertex3f(max[0], max[1], max[2]);

	// z軸平行
	glVertex3f(min[0], min[1], min[2]);	glVertex3f(min[0], max[1], min[2]);
	glVertex3f(min[0], min[1], max[2]);	glVertex3f(min[0], max[1], max[2]);
	glVertex3f(max[0], min[1], min[2]);	glVertex3f(max[0], max[1], min[2]);
	glVertex3f(max[0], min[1], max[2]);	glVertex3f(max[0], max[1], max[2]);
	
	glEnd();

	glPopMatrix();
}


/*!
 * 任意ベクトル周りの回転
 * @param[in] pos 元の座標値
 * @param[in] axis 回転軸
 * @param[in] ang 回転角度(rad)
 * @return 回転した座標値
 */
inline Vec3 Rotate(const Vec3 &pos, const Vec3 &axis, const double &ang)
{
	Vec3 pos1;    // 回転後の座標値

	double c = cos(ang);
	double s = sin(ang);
	double x, y, z;
	x = axis[0]; y = axis[1]; z = axis[2];
	
	// | xx(1-c)+c	xy(1-c)-zs	xz(1-c)+ys	0 |
	// | yx(1-c)-zs	yy(1-c)+c	yz(1-c)-xs	0 |
	// | xz(1-c)-ys	yz(1-c)+xs	zz(1-c)+c	0 |
	// | 0			0			0			1 |
	pos1[0] =   (x*x*(1.0-c)+c)*pos[0] +(x*y*(1.0-c)-z*s)*pos[1] +(x*z*(1.0-c)+y*s)*pos[2];
	pos1[1] = (y*x*(1.0-c)-z*s)*pos[0]   +(y*y*(1.0-c)+c)*pos[1] +(y*z*(1.0-c)-x*s)*pos[2];
	pos1[2] = (x*z*(1.0-c)-y*s)*pos[0] +(y*z*(1.0-c)+x*s)*pos[1]   +(z*z*(1.0-c)+c)*pos[2];
 
	return pos1;
}


/*!
 * 文字列描画
 * @param[in] cir_str 文字列循環バッファ
 * @param[in] static_str 静的な文字列バッファ
 * @param[in] w,h ウィンドウサイズ
 */
void DrawStrings(vector<string> &static_str, int w, int h, int offsetx, int offsety)
{
	glDisable(GL_LIGHTING);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	gluOrtho2D(0, w, 0, h);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	float x0 = offsetx;
	float y0 = h-offsety;

	// glutBitmapStringで文字列描画
	for(int j = 0; j < (int)static_str.size(); ++j){
		glRasterPos2f(x0, y0);
		int size = (int)static_str[j].size();
		for(int i = 0; i < size; ++i){
			char ic = static_str[j][i];
			glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, ic);
		}
		y0 -= 20;
	}

	glRasterPos2f(0, 0);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
}


/*!
 * 文字列描画
 * @param[in] cir_str 文字列循環バッファ
 * @param[in] static_str 静的な文字列バッファ
 * @param[in] w,h ウィンドウサイズ
 */
void DrawStringsBottom(vector<string> &static_str, int w, int h, int offsetx, int offsety)
{
	//w *= 0.5;
	//h *= 0.5;
	
	glDisable(GL_LIGHTING);
	//glColor3f(0.0, 0.0, 0.0);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	gluOrtho2D(0, w, 0, h);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	float x0 = 20.0f;
	float y0 = static_str.size()*20.0f;
	glRasterPos2f(20.0f+offsetx, y0);

	for(int j = 0; j < (int)static_str.size(); ++j){
		glRasterPos2i(20+offsetx, y0);
		int size = (int)static_str[j].size();
		for(int i = 0; i < size; ++i){
			char ic = static_str[j][i];
			glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, ic);
		}

		y0 -= 20;
	}

	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
}





//-----------------------------------------------------------------------------
// 画像の読込
//-----------------------------------------------------------------------------
static int m_iBitmapType = RX_BMP_WINDOWS_V3;
inline void SetBitmapType(int type){ m_iBitmapType = type; }


unsigned char* ReadImageFile(const std::string &fn, int &w, int &h, int &c)
{
	// 拡張子抽出
	string ext;
	size_t pos1 = fn.rfind('.');
	if(pos1 != string::npos){
		ext = fn.substr(pos1+1, fn.size()-pos1);
		string::iterator itr = ext.begin();
		while(itr != ext.end()){
			*itr = tolower(*itr);
			itr++;
		}
		itr = ext.end()-1;
		while(itr != ext.begin()){	// パスの最後に\0やスペースがあったときの対策
			if(*itr == 0 || *itr == 32){
				ext.erase(itr--);
			}
			else{
				itr--;
			}
		}
	}

	// 画像読み込み
	unsigned char* pimg;
	if(ext == "bmp"){
		pimg = ReadBitmapFile(fn, w, h, c);
	}
	//else if(ext == "png"){
	//	pimg = ReadPngFile(fn, w, h, c);
	//}
	//else if(ext == "jpg" || ext == "jpeg"){
	//	pimg = ReadJpegFile(fn, w, h, c);
	//}
	else{
		return 0;
	}

	return pimg;
}


int WriteImageFile(const std::string &fn, unsigned char *img, int w, int h, int c, int quality)
{
	// 拡張子抽出
	string ext;
	size_t pos1 = fn.rfind('.');
	if(pos1 != string::npos){
		ext = fn.substr(pos1+1, fn.size()-pos1);
		string::iterator itr = ext.begin();
		while(itr != ext.end()){
			*itr = tolower(*itr);
			itr++;
		}
		itr = ext.end()-1;
		while(itr != ext.begin()){	// パスの最後に\0やスペースがあったときの対策
			if(*itr == 0 || *itr == 32){
				ext.erase(itr--);
			}
			else{
				itr--;
			}
		}
	}

	// 画像読み込み
	if(ext == "bmp"){
		WriteBitmapFile(fn, img, w, h, c, m_iBitmapType);
	}
	//else if(ext == "png"){
	//	WritePngFile(fn, img, w, h, c);
	//}
	//else if(ext == "jpg" || ext == "jpeg"){
	//	WriteJpegFile(fn, img, w, h, c, quality);
	//}
	else{
		return 0;
	}

	return 1;
}


/*! 
 * フレームバッファのRGB情報を一時的なバッファに確保
 * @retval true 保存成功
 * @retval false 保存失敗
 */
bool SaveFrameBuffer(const string &fn, int w, int h)
{
	vector<unsigned char> imm_buf(w*h*3);

	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, &imm_buf[0]);

	// 上下反転
	int stride = w*3;
	for(int j = 0; j < h/2; ++j){
		for(int i = 0; i < stride; ++i){
			unsigned char tmp = imm_buf[j*stride+i];
			imm_buf[j*stride+i] = imm_buf[(h-j-1)*stride+i];
			imm_buf[(h-j-1)*stride+i] = tmp;
		}
	}

	WriteImageFile(fn, &imm_buf[0], w, h, 3);

	return true;
}


/*! テクスチャをビットマップで保存
	@param[in] file_name 保存ファイル名
	@param[in] tex_obj 保存したいテクスチャオブジェクト
	@param[in] jpeg_quality JPEGで保存する場合の保存品質(0-255)
	@retval true 保存成功
	@retval false 保存失敗
*/
bool SaveTexture(const string &fn, rxTexObj2D &tex_obj, int jpeg_quality)
{
	int w = tex_obj.m_iW;
	int h = tex_obj.m_iH;
	int c = 3;
	vector<unsigned char> imm_buf(w*h*c);

	int ic, jc, idx;
	for(jc = 0; jc < h; ++jc){
		for(ic = 0; ic < w; ++ic){
			idx = 3*(jc*w+ic);

			imm_buf[idx+0] = tex_obj.m_pImage[idx];
			imm_buf[idx+1] = tex_obj.m_pImage[idx+1];
			imm_buf[idx+2] = tex_obj.m_pImage[idx+2];
		}
	}

	WriteImageFile(fn, &imm_buf[0], w, h, c, jpeg_quality);

	return true;
}

/*! 
 * ReadTexture テクスチャの読み込み
 * @param[in] path テクスチャ画像のパス
 * @param[out] tex_obj テクスチャを格納する
 * @return テクスチャが読み込めたかどうか
 */
bool ReadTexture(const string &fn, rxTexObj2D &tex_obj)
{
	int w0, h0, c0;
	unsigned char* pimg;
	pimg = ReadImageFile(fn, w0, h0, c0);
	if(pimg == 0){
		return false;
	}

	tex_obj.SetSize(w0, h0, c0);

	int ic, jc;
	for(jc = 0; jc < tex_obj.m_iH; ++jc){
		for(ic = 0; ic < tex_obj.m_iW; ++ic){
			int idx = 3*(jc*w0+ic);
			tex_obj.SetColor(ic, h0-jc-1, pimg[idx+0], pimg[idx+1], pimg[idx+2]);
		}
	}

	// テクスチャ登録
	tex_obj.Bind();

	// テクスチャパラメータの設定
	tex_obj.SetParameter(GL_TEXTURE_WRAP_S, GL_REPEAT);
	tex_obj.SetParameter(GL_TEXTURE_WRAP_T, GL_REPEAT);
	tex_obj.SetParameter(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	tex_obj.SetParameter(GL_TEXTURE_MIN_FILTER, GL_NEAREST);

	tex_obj.SetTexture();

	return true;
}


/*! 
 * 環境マップ用のキューブマップテクスチャの読み込み
 * @param[in] fn[6] テクスチャ画像(6枚)のパス(x+,x-,y+,y-,z+,z-)(右,左,上,下,後,前)
 * @param[out] cube_map rxCubeMapData型
 * @retval true  キューブマップ用画像の読み込み成功
 * @retval false キューブマップ用画像の読み込み失敗
 */
bool LoadCubeMapTexture(const string fn[6], rxCubeMapData &cube_map)
{
	GLuint tex_name;
	glGenTextures(1, &tex_name);
	glBindTexture(GL_TEXTURE_CUBE_MAP, tex_name);

	// キューブマップテクスチャパラメータの設定
	glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);		// 画像境界の扱いの指定
	glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);	// 画像フィルタの指定
	glTexParameterf(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, 6);

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	
	GLenum target[6] = { GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_CUBE_MAP_NEGATIVE_X, 
						 GL_TEXTURE_CUBE_MAP_POSITIVE_Y, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, 
						 GL_TEXTURE_CUBE_MAP_POSITIVE_Z, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z };

	for(int i = 0; i < 6; ++i){
		int w, h, c;
		unsigned char* pimg;
		pimg = ReadImageFile(fn[i], w, h, c);
		if(!pimg){
			return false;
		}

		GLuint format;
		format = GL_RGBA;
		if(c == 1){
			format = GL_LUMINANCE;
		}
		else if(c == 3){
			format = GL_RGB;
		}

		GLuint iformat;
		iformat = GL_RGBA;
		if(c == 1){
			iformat = GL_LUMINANCE;
		}
		else if(c == 3){
			iformat = GL_RGB;
		}

		gluBuild2DMipmaps(target[i], format, w, h, iformat, GL_UNSIGNED_BYTE, pimg); 


		free(pimg);	
	}

	glBindTexture(GL_TEXTURE_2D, 0);	

	cube_map.id = tex_name;

	return true;
}

/*! 
 * 環境マップ用のキューブマップテクスチャの読み込み
 * @param cube_map キューブマップデータ
 * @param base キューブマップ用画像のファイル名のベース部分
 * @param ext キューブマップ用画像のファイルの拡張子
 * @retval true  キューブマップ用画像の読み込み成功
 * @retval false キューブマップ用画像の読み込み失敗
 */
bool LoadCubeMap(rxCubeMapData &cube_map, string base, string ext)
{
	// キューブマップ用画像の読み込み(x+,x-,y+,y-,z+,z-)(右,左,上,下,後,前)
	string fn[6];
	fn[0] = base+"posx"+ext;
	fn[1] = base+"negx"+ext;
	fn[2] = base+"posy"+ext;
	fn[3] = base+"negy"+ext;
	fn[4] = base+"posz"+ext;
	fn[5] = base+"negz"+ext;

	if(!LoadCubeMapTexture(fn, cube_map)){
		return false;
	}

	return true;
}
