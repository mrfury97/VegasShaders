#pragma once

// UNOFFICIAL (BounceLight: Temporal, MultiBounce): the matrix that takes a point from this frame's view space to last
// frame's clip space (BounceLight.fx.hlsl's NVR_BounceReproject), for finding last frame's bounce at the same spot.
// From the two frames' view matrices and their inverses (NVR's: world -> view, the camera's world position in the
// translation; TESR_ViewTransform, TESR_InvViewTransform) and last frame's projection (TESR_ProjectionTransform).
// Worked out in double precision round the two camera positions: far from the world's origin (the Mojave's coordinates
// run past 100 000 units) the few units the camera moves between two frames would otherwise be lost to float rounding.
// Header only, no game types, so the bench (work\bench\bounce-gi) uses the same code.
inline void BounceLightReprojection(const D3DXMATRIX& viewNow, const D3DXVECTOR4& camPosNow,
	const D3DXMATRIX& viewLast, const D3DXVECTOR4& camPosLast, const D3DXMATRIX& projLast, D3DXMATRIX* result) {
	double rotation[3][3];
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
		double sum = 0.0;
		for (int k = 0; k < 3; k++) sum += (double)viewNow.m[k][i] * (double)viewLast.m[k][j];
		rotation[i][j] = sum;
	}
	const double move[3] = { (double)camPosNow.x - camPosLast.x, (double)camPosNow.y - camPosLast.y,
		(double)camPosNow.z - camPosLast.z };
	double translation[3];
	for (int j = 0; j < 3; j++) translation[j] = move[0] * viewLast.m[0][j] + move[1] * viewLast.m[1][j] + move[2] * viewLast.m[2][j];
	D3DXMATRIX toLastView;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) toLastView.m[i][j] = (float)rotation[i][j];
		toLastView.m[i][3] = 0.0f;
		toLastView.m[3][i] = (float)translation[i];
	}
	toLastView.m[3][3] = 1.0f;
	D3DXMatrixMultiply(result, &toLastView, &projLast);
}

inline void BounceLightReprojection(const D3DXMATRIX& viewNow, const D3DXMATRIX& invViewNow,
	const D3DXMATRIX& viewLast, const D3DXMATRIX& invViewLast, const D3DXMATRIX& projLast, D3DXMATRIX* result) {
	BounceLightReprojection(viewNow, D3DXVECTOR4(invViewNow._41, invViewNow._42, invViewNow._43, 1.0f),
		viewLast, D3DXVECTOR4(invViewLast._41, invViewLast._42, invViewLast._43, 1.0f), projLast, result);
}
