#version 450
// A triangle that covers the target: vertices (-1,-1), (3,-1), (-1,3). The
// viewport is the frame's place on it, and the fragment shader works from
// gl_FragCoord.
void main()
{
    gl_Position = vec4(float((gl_VertexIndex << 1) & 2) * 2.0 - 1.0, float(gl_VertexIndex & 2) * 2.0 - 1.0, 0.0, 1.0);
}
