target_sources(${PROJECT_NAME} PRIVATE
        addrspace.cpp
        addrspace.h
        mem_watch.cpp
        mem_watch.h
        # gdxsv: VRAM protection owners and write tracker
        vram_protect.cpp
        vram_protect.h)
