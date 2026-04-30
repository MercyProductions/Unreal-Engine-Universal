set_project("Dumper-7")
add_rules("mode.debug", "mode.release")
set_languages("c++latest", "clatest")

target("Dumper-7")
    set_kind("shared")

    add_files("Dumper/**.cpp")
    add_files("Dumper/**.c")
    add_files("imgui-master/imgui.cpp")
    add_files("imgui-master/imgui_draw.cpp")
    add_files("imgui-master/imgui_tables.cpp")
    add_files("imgui-master/imgui_widgets.cpp")
    add_files("imgui-master/backends/imgui_impl_dx11.cpp")
    add_files("imgui-master/backends/imgui_impl_dx12.cpp")
    add_files("imgui-master/backends/imgui_impl_opengl3.cpp")
    add_files("imgui-master/backends/imgui_impl_win32.cpp")

    add_includedirs("Dumper", {public = true})
    add_includedirs("Dumper/Utils", {public = true})
    add_includedirs("Dumper/Engine/Public", {public = true})
    add_includedirs("Dumper/Generator/Public", {public = true})
    add_includedirs("Dumper/Platform/Public", {public = true})
    add_includedirs("imgui-master", {public = true})
    add_includedirs("imgui-master/backends", {public = true})

    add_cxflags("/wd4244", "/wd4267", "/wd4369", "/wd4715")

    add_links(
        "kernel32", "user32", "gdi32", "winspool", "comdlg32",
        "advapi32", "shell32", "ole32", "oleaut32", "uuid",
        "d3d11", "d3d12", "dcomp", "dxgi", "dwmapi", "opengl32",
        "odbc32", "odbccp32", "ntdll"
    )

    if is_mode("release") then
        set_runtimes("MD")
        set_targetdir("Bin/Release/")
        set_objectdir("Bin/Intermediates/Release/.objs")
        set_dependir("Bin/Intermediates/Release/.deps")
    else
        set_runtimes("MDd")
        set_targetdir("Bin/Debug/")
        set_objectdir("Bin/Intermediates/Debug/.objs")
        set_dependir("Bin/Intermediates/Debug/.deps")
    end
