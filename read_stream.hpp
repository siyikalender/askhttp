//
// Authors: Selçuk İyikalender
//          Claude (Anthropic)
//
// SPDX-License-Identifier: Unlicense
//
// read_whole_stream(): reads a seekable stream (e.g. a std::ifstream opened
// in binary mode) completely into a container such as std::string.
//

#ifndef ASKHTTP_READ_STREAM_HPP
#define ASKHTTP_READ_STREAM_HPP

#include <iostream>

template<typename RandomAccessibleContainer>
inline void read_whole_stream(std::istream &stream, RandomAccessibleContainer &content)
{
  auto begin = stream.tellg();
  stream.seekg(0, std::ios::end);
  auto end   = stream.tellg();
  stream.seekg(0, std::ios::beg);
  content.resize(end - begin);
  stream.read(&content[0], content.size());
}

// ASKHTTP_READ_STREAM_HPP
#endif
