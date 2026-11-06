#pragma once

// Source-owned regression EXEs use the same image-owned .compat arrangement
// as the real bootstrap. Windows reserves their ABI addresses before allocating
// process heaps/stacks; tests never race VirtualAlloc against those allocations.
bool AdoptCanonicalTestArena();
