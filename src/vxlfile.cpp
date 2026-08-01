#include "vxlfile.h"
#include "normal_table.h"
#include "vplfile.h"

CLASSES_START

size_t vxlfile::direction_count = 32u;

d3dvector vxlfile::reversed_light = { 0.0f,1.0f,0.0f };

hvafile::hvafile() :_signature(),
	_framecount(0),
	_sectioncount(0),
	_frame_matrices()
{}

hvafile::hvafile(const std::string& filename) :hvafile()
{
	load(filename);
}

hvafile::hvafile(const byte* buffer) : hvafile()
{
	load(buffer);
}

void hvafile::clear()
{
	_frame_matrices.clear();
	_framecount = _sectioncount = 0;
}

bool hvafile::load(const std::string& filename)
{
	std::ifstream file(filename, std::ios::in | std::ios::binary);
	if (!file)
		return false;

	std::vector<byte> file_buffer;
	size_t filesize = static_cast<size_t>(file.seekg(0, std::ios::end).tellg());

	file.seekg(0, std::ios::beg);
	file_buffer.resize(filesize);
	file.read(reinterpret_cast<char*>(file_buffer.data()), filesize);

	return load(file_buffer.data());
}

bool hvafile::load(const byte* buffer)
{
	if (!buffer)
		return false;

	clear();

	byte* bytes = const_cast<byte*>(buffer);
	size_t current_offset = 0;
	
	memcpy(_signature, bytes, sizeof(_signature));
	bytes += sizeof(_signature);
	memcpy(&_framecount, bytes, sizeof(_framecount));
	bytes += sizeof(_framecount);
	memcpy(&_sectioncount, bytes, sizeof(_sectioncount));
	bytes += sizeof(_sectioncount);

	_frame_matrices.resize(frame_count() * section_count());
	memcpy(_frame_matrices.data(), bytes + section_count() * sizeof(_signature), _frame_matrices.size() * sizeof(game_matrix));

	return true;
}

bool hvafile::is_loaded()
{
	return !_frame_matrices.empty();
}

size_t hvafile::frame_count()
{
	return _framecount;
}

size_t hvafile::section_count()
{
	return _sectioncount;
}

game_matrix& hvafile::matrix(size_t frame, size_t section)
{
	return _frame_matrices[frame * section_count() + section];
}

void vxlfile::clear()
{
	_body_data.clear();
	_headers.clear();
	_tailers.clear();
	_associated_hva.reset();

	_prepared_vertex.clear();
	_cache.clear();
	_shadow_cache.clear();
}

bool vxlfile::is_loaded()
{
	return !_body_data.empty() && _associated_hva;
}

bool vxlfile::load(const std::string& filename)
{
	std::ifstream file(filename, std::ios::in | std::ios::binary);
	if (!file)
		return false;

	std::vector<byte> filebuffer;
	size_t filesize = static_cast<size_t>(file.seekg(0, std::ios::end).tellg());
	file.seekg(0, std::ios::beg);

	filebuffer.resize(filesize);
	file.read(reinterpret_cast<char*>(filebuffer.data()), filesize);
	
	return load(filebuffer.data());

}

bool vxlfile::load(const byte* buffer)
{
	if (!buffer)
		return false;

	clear();

	byte* floating_cur = const_cast<byte*>(buffer);

	memcpy(&_fileheader, floating_cur, sizeof(_fileheader));
	for (color& color : _fileheader.internal_palette)
	{
		color.r <<= 2;
		color.g <<= 2;
		color.b <<= 2;
	}

	size_t limb_count = _fileheader.limb_count;
	_body_data.resize(limb_count);
	_headers.resize(limb_count);
	_tailers.resize(limb_count);

	floating_cur += sizeof(_fileheader);
	memcpy(_headers.data(), floating_cur, limb_count * sizeof(vxl_limb_header));

	floating_cur += limb_count * sizeof(vxl_limb_header) + _fileheader.body_size;
	memcpy(_tailers.data(), floating_cur, limb_count * sizeof(vxl_limb_tailer));

	floating_cur -= _fileheader.body_size;
	for (size_t i = 0; i < limb_count; i++)
	{
		vxl_limb_header& current_header = _headers[i];
		vxl_limb_tailer& current_tailer = _tailers[i];
		vxl_limb& current_body = _body_data[i];

		size_t span_count = current_tailer.xsize * current_tailer.ysize;
		current_body.span_data_blocks.resize(span_count);
		current_body.span_ends.resize(span_count);
		current_body.span_starts.resize(span_count);

		byte* span_data_blocks = floating_cur + current_tailer.span_data_offset;
		byte* span_start_data = floating_cur + current_tailer.span_start_offset;
		byte* span_end_data = floating_cur + current_tailer.span_end_offset;

		memcpy(current_body.span_starts.data(), span_start_data, span_count * sizeof(uint32_t));
		memcpy(current_body.span_ends.data(), span_end_data, span_count * sizeof(uint32_t));

		for (size_t n = 0; n < span_count; n++)
		{
			span_data& current_span = current_body.span_data_blocks[n];
			current_span.voxels_size = 0;
			current_span.voxels.resize(current_tailer.zsize);
			
			if (current_body.span_starts[n] == 0xffffffffu || current_body.span_ends[n] == 0xffffffffu)
				continue;

			byte* current_span_data = span_data_blocks + current_body.span_starts[n];
			byte* current_span_end = span_data_blocks + current_body.span_ends[n];

			size_t current_vox_idx = 0;
			byte skip_count, voxel_count, voxel_end;

			do
			{
				skip_count = *current_span_data++;
				voxel_count = *current_span_data++;
				current_vox_idx += skip_count;

				if (current_vox_idx >= current_span.voxels.size())
					break;

				if (voxel_count)
				{
					size_t actual_count = voxel_count;
					if (current_vox_idx + actual_count > current_span.voxels.size())
						actual_count = current_span.voxels.size() - current_vox_idx;

					memcpy(&current_span.voxels[current_vox_idx], current_span_data, actual_count * sizeof(voxel));
					current_span_data += voxel_count * sizeof(voxel);
				}			
				current_vox_idx += voxel_count;
				voxel_end = *current_span_data++;
			} 			
			while (current_span_data <= current_span_end);

			for (size_t z = 0; z < current_tailer.zsize; z++)
			{
				if (current_span.voxels[z].color)
					current_span.voxels_size++;
			}
		}
	}

	return true;

}

bool vxlfile::load_hva(const std::string& filename)
{
	_associated_hva.reset();
	_associated_hva = std::make_unique<hvafile>(filename);
	return _associated_hva->is_loaded();
}

bool vxlfile::load_hva(const byte* buffer)
{
	_associated_hva.reset();
	_associated_hva = std::make_unique<hvafile>(buffer);
	return _associated_hva->is_loaded();
}

bool vxlfile::prepare_vertecies(const size_t limb)
{
	if (!is_loaded() || limb >= limb_count())
		return false;

	if (vertecies(limb).size())
		vertecies(limb).clear();

	vxl_vertex vertex;
	vxl_limb_tailer& first_tailer = _tailers[limb];

	for (uint32_t x = 0; x < first_tailer.xsize; x++)
	{
		for (uint32_t y = 0; y < first_tailer.ysize; y++)
		{
			for (uint32_t z = 0; z < first_tailer.zsize; z++)
			{
				voxel vox = voxel_rh(limb, x, y, z);
				
				if (vox.color)
				{
					vertex.position.x = static_cast<float>(x);
					vertex.position.y = static_cast<float>(y);
					vertex.position.z = static_cast<float>(z);
					vertex.voxel = vox;

					vertecies(limb).push_back(vertex);
				}
			}
		}
	}

	return true;
}

vxlfile::vertex_cache_type& vxlfile::vertecies(const size_t limb)
{
	return _prepared_vertex[limb];
}

bool vxlfile::prepare_single_dir_cache(
	const size_t diridx,
	vplfile& vplfile,
	const int F, const int L, const int H,
	const int fire_angle,
	const bool VanillaShadow,
	const float tilt_angle,
	const float tilt_direction)
{
	if (!is_loaded() || diridx >= direction_count)
		return false;

	const rect buffer_view = { 0,0,buffer_width,buffer_height };

	std::unique_ptr<byte[]> cache(new byte[buffer_width * buffer_height]);
	std::unique_ptr<byte[]> shadow_cache(new byte[buffer_width * buffer_height]);
	std::unique_ptr<float[]> zbuffer(new float[buffer_width * buffer_height]);

	if (!cache || !shadow_cache || !zbuffer)
		return false;

	memset(cache.get(), 0, buffer_width * buffer_height);
	memset(shadow_cache.get(), 0, buffer_width * buffer_height);
	std::fill_n(zbuffer.get(), buffer_width * buffer_height, std::numeric_limits<float>::max());

	d3dmatrix off, rotation, rotationY, rotationTilt;

	float rotation_angle = float((diridx + direction_count / 2) % direction_count * D3DX_PI * 2.0 / direction_count);
	float rotation_angle_fire = float(fire_angle / 64.0f * 90.0f / 360.0f * D3DX_PI * 2.0f);

	D3DXMatrixTranslation(&off,
		float(F * 30.0 * D3DX_SQRT2 / 256.0),
		float(L * 30.0 * D3DX_SQRT2 / 256.0),
		float(H * 30.0 * D3DX_SQRT2 / 256.0));

	D3DXMatrixRotationZ(&rotation, rotation_angle);
	D3DXMatrixRotationY(&rotationY, rotation_angle_fire);

	float sin_a = sinf(tilt_angle);
	float cos_a = cosf(tilt_angle);
	float sin_d = sinf(tilt_direction);
	float cos_d = cosf(tilt_direction);
	float c = cos_a;
	float s = -sin_a;
	float one_minus_c = 1.0f - c;
	D3DXMatrixIdentity(&rotationTilt);
	rotationTilt.m[0][0] = 1.0f - one_minus_c * cos_d * cos_d;
	rotationTilt.m[0][1] = -one_minus_c * cos_d * sin_d;
	rotationTilt.m[0][2] = s * cos_d;
	rotationTilt.m[1][0] = -one_minus_c * sin_d * cos_d;
	rotationTilt.m[1][1] = 1.0f - one_minus_c * sin_d * sin_d;
	rotationTilt.m[1][2] = s * sin_d;
	rotationTilt.m[2][0] = -s * cos_d;
	rotationTilt.m[2][1] = -s * sin_d;
	rotationTilt.m[2][2] = c;

	float shadow_nx = cos_d * sin_a;
	float shadow_ny = sin_d * sin_a;
	float shadow_nz = cos_a;

	int xl = buffer_width - 1, xh = 0;
	int yl = buffer_height - 1, yh = 0;

	int sxl = buffer_width - 1, sxh = 0;
	int syl = buffer_height - 1, syh = 0;

	if (_prepared_vertex.empty())
	{
		_prepared_vertex.resize(limb_count());
		for (size_t i = 0; i < limb_count(); i++)
			prepare_vertecies(i);
	}

	D3DXVec3Normalize(&reversed_light, &reversed_light);

	float min_bound_z = 256.0f;
	for (size_t l = 0; l < limb_count(); l++)
	{
		auto& t = _tailers[l];

		d3dvector scales{
			(t.max_bounds.x() - t.min_bounds.x()) / t.xsize,
			(t.max_bounds.y() - t.min_bounds.y()) / t.ysize,
			(t.max_bounds.z() - t.min_bounds.z()) / t.zsize
		};

		d3dmatrix transform = _associated_hva->matrix(0, l).integrate_matrix(scales, t.scale);
		min_bound_z = std::min(min_bound_z, t.min_bounds.z() + transform.m[3][2]);
	}

	float shadow_ground = cos_a * (VanillaShadow ? min_bound_z : 0.0f);

	for (size_t l = 0; l < limb_count(); l++)
	{
		auto& vertex_cache = vertecies(l);
		if (vertex_cache.empty()) continue;

		auto& t = _tailers[l];

		d3dvector scales{
			(t.max_bounds.x() - t.min_bounds.x()) / t.xsize,
			(t.max_bounds.y() - t.min_bounds.y()) / t.ysize,
			(t.max_bounds.z() - t.min_bounds.z()) / t.zsize
		};

		d3dmatrix transform = _associated_hva->matrix(0, l).integrate_matrix(scales, t.scale);

		vector3<float> center = t.min_bounds;
		center.x() /= scales.x;
		center.y() /= scales.y;
		center.z() /= scales.z;

		d3dmatrix trans_center = math::translation_from(center);

		d3dmatrix scale;
		D3DXMatrixScaling(&scale, scales.x, scales.y, scales.z);

		d3dmatrix mirrorX;
		D3DXMatrixScaling(&mirrorX, -1.0f, 1.0f, 1.0f);

		d3dmatrix result =
			trans_center *
			scale *
			transform *
			off *
			mirrorX *
			rotationY *
			rotation *
			rotationTilt;

		auto* normal_table = normal::normal_table_directory[(uint8_t)t.normal_type];

		uint8_t normalLightIndex[256];

		for (int i = 0; i < 256; i++)
		{
			D3DXVECTOR3 n;
			D3DXVec3TransformNormal(&n, &normal_table[i], &result);
			D3DXVec3Normalize(&n, &n);

			float dot = n.x * reversed_light.x +
				n.y * reversed_light.y +
				n.z * reversed_light.z;

			dot = std::clamp(dot, -1.0f, 1.0f);

			float angle = acosf(dot);

			int idx;
			if (angle >= D3DX_PI / 2.0f)
				idx = 0;
			else
				idx = 31 - int(angle / (D3DX_PI / 2.0f) * 32.0f);

			normalLightIndex[i] = (uint8_t)idx;
		}

		for (auto& vertex : vertex_cache)
		{
			const auto& v = vertex.position;

			float px =
				result.m[0][0] * v.x +
				result.m[1][0] * v.y +
				result.m[2][0] * v.z +
				result.m[3][0];

			float py =
				result.m[0][1] * v.x +
				result.m[1][1] * v.y +
				result.m[2][1] * v.z +
				result.m[3][1];

			float pz =
				result.m[0][2] * v.x +
				result.m[1][2] * v.y +
				result.m[2][2] * v.z +
				result.m[3][2];

			D3DXVECTOR3 pos{ px, py, pz };

			D3DXVECTOR3 sp = math::fructum_transformation(buffer_view, pos);

			int x = (int)sp.x;
			int y = (int)sp.y;

			if ((unsigned)x < buffer_width && (unsigned)y < buffer_height)
			{
				byte* cache_row = &cache[y * buffer_width];
				float* z_row = &zbuffer[y * buffer_width];

				float z = sp.z;

				if (vertex.voxel.color && z >= 0.0f)
				{
					if (z < z_row[x])
					{
						z_row[x] = z;

						uint8_t lightIdx = normalLightIndex[vertex.voxel.normal];
						cache_row[x] = vplfile[lightIdx][vertex.voxel.color];

						xl = std::min(xl, x);
						xh = std::max(xh, x);
						yl = std::min(yl, y);
						yh = std::max(yh, y);
					}
				}
			}

			// 阴影：顶点沿斜坡法向量方向(n)投影到斜坡平面上
			// 交点 s = p + (shadow_ground - n·p) * n
			float dot_np = shadow_nx * px + shadow_ny * py + shadow_nz * pz;
			float t = shadow_ground - dot_np;
			float shadow_wx = px + t * shadow_nx;
			float shadow_wy = py + t * shadow_ny;
			float shadow_wz = pz + t * shadow_nz;

			D3DXVECTOR3 sp2 = math::fructum_transformation(buffer_view, { shadow_wx, shadow_wy, shadow_wz });

			int sx = (int)sp2.x;
			int sy = (int)sp2.y;

			if ((unsigned)sx < buffer_width && (unsigned)sy < buffer_height)
			{
				shadow_cache[sy * buffer_width + sx] = 1;

				sxl = std::min(sxl, sx);
				sxh = std::max(sxh, sx);
				syl = std::min(syl, sy);
				syh = std::max(syh, sy);
			}
		}
	}

	size_t width = xh - xl + 1;
	size_t height = yh - yl + 1;

	size_t sw = sxh - sxl + 1;
	size_t sh = syh - syl + 1;

	cache_frame clip(new byte[width * height]);
	cache_frame clip_shadow(new byte[sw * sh]);

	for (int y = yl; y <= yh; y++)
		memcpy(&clip[(y - yl) * width], &cache[y * buffer_width + xl], width);

	for (int y = syl; y <= syh; y++)
		memcpy(&clip_shadow[(y - syl) * sw], &shadow_cache[y * buffer_width + sxl], sw);

	if (_cache.empty()) _cache.resize(direction_count);
	if (_shadow_cache.empty()) _shadow_cache.resize(direction_count);

	_cache[diridx] = { clip, { xl,yl,xh + 1,yh + 1 } };
	_shadow_cache[diridx] = { clip_shadow, { sxl,syl,sxh + 1,syh + 1 } };

	return true;
}

bool vxlfile::prepare_all_cache(vplfile& vplfile, const int F, const int L, const int H)
{
	bool result = true;
	for (size_t i = 0; i < direction_count; i++)
	{
		result &= prepare_single_dir_cache(i, vplfile, F, L, H);
	}

	return result;
}

vxlfile::cache_storage vxlfile::frame_cache(const size_t direction)
{
	cache_storage result;
	if (direction >= _cache.size())
		return result;
	return _cache[direction];
}

vxlfile::cache_storage vxlfile::shadow_cache(const size_t direction)
{
	cache_storage result;
	if (direction >= _shadow_cache.size())
		return result;
	return _shadow_cache[direction];
}

size_t vxlfile::limb_count()
{
	return is_loaded() ? _fileheader.limb_count : 0u;
}

size_t vxlfile::frame_count()
{
	return is_loaded() ? _associated_hva->frame_count() : 0u;
}

size_t vxlfile::section_count()
{
	return is_loaded() ? _associated_hva->section_count() : 0u;
}

voxel vxlfile::voxel_lh(size_t limb, uint32_t x, uint32_t y, uint32_t z)
{
	return voxel_rh(limb, y, x, z);
}

voxel vxlfile::voxel_rh(size_t limb, uint32_t x, uint32_t y, uint32_t z)
{
	voxel result;
	if (!is_loaded() || limb >= limb_count())
		return result;

	vxl_limb_tailer& tailer = _tailers[limb];
	vxl_limb& body = _body_data[limb];

	if (x >= tailer.xsize || y >= tailer.ysize || z >= tailer.zsize)
		return result;

	return body.span_data_blocks[y * tailer.xsize + x].voxels[z];
}

void vxlfile::print_info()
{
#if 0
	if (!is_loaded())
		std::cout << "Current vxl is not loaded.\n";

	std::cout << "Number of limbs : " << limb_count() << ".\n";
	for (size_t i = 0; i < limb_count(); i++)
	{
		vxl_limb_tailer& current_tailer = _tailers[i];
		vxl_limb& current_body = _body_data[i];

		std::cout << std::hex
			<< "Span start offset = " << current_tailer.span_start_offset << ".\n"
			<< "End offset = " << current_tailer.span_end_offset << ".\n"
			<< "Data offset = " << current_tailer.span_data_offset << ".\n"
			<< std::dec;

		std::cout << "Vxl dimensiton = " << (size_t)current_tailer.xsize << ", " << (size_t)current_tailer.ysize << ", " << (size_t)current_tailer.zsize << ".\n";
		for (size_t x = 0; x < current_tailer.xsize; x++)
		{
			for (size_t y = 0; y < current_tailer.ysize; y++)
			{
				span_data& span = current_body.span_data_blocks[y * current_tailer.xsize + x];
				std::cout << "Span coords = " << x << ", " << y << ", voxel count = " << (uint32_t)span.voxels_size << ".\n";

				for (auto& voxel : span.voxels)
				{
					std::cout << "(" << (int)voxel.color << ", " << (int)voxel.normal << "), ";
				}
				std::cout << std::endl;
			}
		}
		std::cout << "About to print info for next limb:\nPress any key to continue.\n";
		//std::cin.get();
	}
#endif
}

CLASSES_END
